#include <paging.h>
#include <memory.h>
#include <frames.h>
#include <exception.h>
#include <utils.h>
#include <scrn.h>
#include <irq.h>
#include <tasks.h>
#include <proc.h>

#define PAGE_BW 12

// Kernel heap. Eventually there will be a user one for the
// processes. The kernel gets its memory from here
static kmem_map_header* kernel_heap = NULL;

#define KHEAP_START 0xC0000000
#define KHEAP_INISIZE 0x400000
#define KHEAP_MAXIMUM 0xDFFFF000

// Number of tables we define initially. It is the simplest
// solution for the problem of needing page tables to
// map to the kernel directory when we append memory to the
// kernel heap: we allocate them in advance and hope they are enough.
#define TABLES_ID_MAP 8

page_directory *current_directory, *kernel_dir;

kmem_map_header* get_kernel_heap()
{
    return kernel_heap;
}

// Returns the physical address corresponding to a
// virtual address given the page directory.
// PRE: The directory tables must be mapped
// (Considering that this runs in kernel mode and that
// page management is owned by it,
// it does not seem unreasonable to me)
uint physical_address(page_directory* pd, uint va)
{
    uint pdi = PAGE_DIR(va), pti = PAGE_TABLE(va), offset = PAGE_OFFSET(va);
    if (!pd->tables_phys[pdi].present) {
        return (uint)-1;
    }
    page_table* p = pd->tables_virtual[pdi];
    if (!p->entries[pti].present) {
        return (uint)-1;
    }
    return (p->entries[pti].frame << PAGE_BW) + offset;
}

void set_current_directory(page_directory* dir)
{
    current_directory = dir;
}

// Enables paging by activating dir as the current directory
void switch_page_directory(page_directory* dir)
{
    uint eflags = irq_cli();
    set_current_directory(dir);
    __asm__ __volatile__("mov %0, %%cr3" ::"r"(dir->physical_address));
    uint cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= PAGING_ENABLED_MASK | WP_MASK;
    // so that it throws a page fault if the
    // kernel tries to access a read-only
    // location
    __asm__ __volatile__("mov %0, %%cr0" ::"r"(cr0));
    irq_sti(eflags);
}

// Next position to request a page table
static intptr page_tables_vs;

// Initializes the frame manager (internally uses a bitmap)
static uint frames_init(intptr start_address, intptr memory_end)
{
    uint used_upper_mem = 0, megabyte = 1024 * 1024;
    uint total_upper_mem = memory_end - start_address;
    if (start_address > megabyte) {
        used_upper_mem = start_address - megabyte;
    }
    uint available_frames = (total_upper_mem - used_upper_mem) / 0x1000;
    return frame_alloc_init((void*)start_address, available_frames);
}

// Maps table_index to a frame (which is expected to be consecutive because this
// function runs at the beginning to generate the kernel memory map prior
// to enabling paging, since afterwards the heap is used for that).
void create_table_entry(page_directory* kernel_directory, uint table_index)
{
    intptr table_dir = frame_alloc(); // Since this runs at the beginning,
    // frame_alloc returns consecutive frames.
    if (table_dir != page_tables_vs) {
        kernel_panic("Kernel pages not contiguous\n");
    }
    page_table_entry table_entry;
    table_entry.present = 1;
    table_entry.read_write = 1;
    table_entry.user = 1; // Table-level protection does not make sense
    table_entry.frame = table_dir >> 12;
    page_tables_vs += PAGE_SZ;
    kernel_directory->tables_phys[table_index] = table_entry;
    kernel_directory->tables_virtual[table_index] = (page_table*)table_dir;
}

// Extends the kernel memory map passed as a parameter, indicating
// the number of pages we want to add at the end.
void* paging_append_core(kmem_map_header* mh, uint pages)
{
    void* prev = (void*)mh->heap_end;
    while (pages-- > 0) {
        if (mh->heap_end >= KHEAP_MAXIMUM) {
            kernel_panic("Heap size exceeded");
        }
        uint frame = frame_alloc();
        // The table always exists since we pre-allocate it before starting.
        // We waste memory but we ensure that all processes will
        // be able to see the kernel heap without major problems
        map_page(kernel_dir, mh->heap_end, frame, PAGEF_P | PAGEF_RW);
        mh->heap_end += PAGE_SZ;
    }
    return prev;
}

static page_table* clone_table(page_table* src)
{
    page_table* pt;
    pt = kmem_alloc_aligned(kernel_heap, sizeof(page_table));
    if (pt == NULL) {
        return NULL;
    }
    memset(pt, 0, sizeof(page_table));
    page_entry *srcpg = src->entries, *dstpg = pt->entries;
    for (uint i = 0; i < PAGE_FRAMES; i++) {
        if (!srcpg[i].present || !srcpg[i].user) {
            continue;
        }
        memcpy(&dstpg[i], &srcpg[i], sizeof(page_entry));
        dstpg[i].read_write = 0;
        if (srcpg[i].read_write) {
            srcpg[i].read_write = 0;
            srcpg[i].copy_on_write = 1;
            dstpg[i].copy_on_write = 1;
        }
        frame_add_alias(srcpg[i].frame << 12);
    }
    return pt;
}

// Clones a directory: Used for forking.
page_directory* clone_directory(page_directory* src)
{
    page_directory* p;
    p = kmem_alloc_aligned(kernel_heap, sizeof(page_directory));
    if (p == NULL) {
        return NULL;
    }
    memset(p, 0, sizeof(page_directory));
    p->physical_address = physical_address(kernel_dir, (intptr)p);
    for (uint i = 0; i < PAGE_TABLES; i++) {
        if (!src->tables_phys[i].present) {
            continue;
        }
        if (kernel_dir->tables_virtual[i] == src->tables_virtual[i]) {
            // Kernel page: We do not want to copy it, we want to link it
            p->tables_phys[i] = src->tables_phys[i];
            p->tables_virtual[i] = src->tables_virtual[i];
        } else {
            // User page: Now we do want to copy it
            p->tables_virtual[i] = clone_table(src->tables_virtual[i]);
            p->tables_phys[i].frame =
                    physical_address(kernel_dir,
                                     (intptr)p->tables_virtual[i]) >>
                    PAGE_BW;
            p->tables_phys[i].present = 1;
            p->tables_phys[i].read_write = 1;
            // We can have tables that are kernel ones:
            // Potentially those of the kernel stack
            p->tables_phys[i].user = src->tables_phys[i].user;
        }
    }
    return p;
}

extern uint signal_handlers_start;
extern uint signal_handlers_end;

// Initializes paging given the last address of the kernel code and
// the last physical address given by the memory
void paging_init(intptr end_address, intptr memory_end)
{
    end_address = NEXT_ALIGN(end_address);
    kernel_dir = (page_directory*)end_address;
    memset(kernel_dir, 0, sizeof(page_directory));
    kernel_dir->physical_address = (intptr)kernel_dir;
    end_address += sizeof(page_directory);
    end_address = NEXT_ALIGN(end_address);
    // We create the manager for the frames.
    end_address = frames_init(end_address, memory_end);
    // We set the start of the tables at page_tables_vs
    page_tables_vs = end_address;
    uint page;
    // We allocate the tables needed for what we have of
    // kernel so far.
    for (page = 0; page < end_address; page += PAGE_SZ) {
        if (!kernel_dir->tables_virtual[PAGE_DIR(page)]) {
            create_table_entry(kernel_dir, PAGE_DIR(page));
        }
    }
    // We allocate the tables needed for all the pages of the heap.
    // We waste some memory but we ensure that everything will be visible.
    for (page = KHEAP_START; page < KHEAP_MAXIMUM; page += PAGE_SZ) {
        if (!kernel_dir->tables_virtual[PAGE_DIR(page)]) {
            create_table_entry(kernel_dir, PAGE_DIR(page));
        }
    }
    // I map a certain number of entries in a fixed way so we do not have
    // troubles mapping the tables for addresses (by mapping 4 tables we have
    // mapped the tables for the first 8*4M = 32 MB of space,
    // more than enough to allocate page table frames for the heap.
    for (uint table = 0; table < TABLES_ID_MAP; table++) {
        if (!kernel_dir->tables_virtual[table]) {
            create_table_entry(kernel_dir, table);
        }
    }
    end_address = page_tables_vs;
    // We do identity mapping of the kernel: Everything needed is now
    // accessible transparently.
    for (page = 0; page < end_address; page += PAGE_SZ) {
        map_page(kernel_dir, page, page, PAGEF_P | PAGEF_RW);
    }
    // The signal handlers are user ones so we map them as
    // user. We use linker symbols to find them
    for (page = (intptr)&signal_handlers_start;
         page < (intptr)&signal_handlers_end; page += PAGE_SZ) {
        map_page(kernel_dir, page, page, PAGEF_P | PAGEF_U);
    }
    // Now we map the initial kernel heap
    for (page = KHEAP_START; page < KHEAP_START + KHEAP_INISIZE;
         page += PAGE_SZ) {
        uint frame = frame_alloc();
        map_page(kernel_dir, page, frame, PAGEF_P | PAGEF_RW);
    }
    register_exception_handler(page_fault_handler, 14);
    switch_page_directory(kernel_dir);
    kernel_heap = kmem_init((void*)KHEAP_START, KHEAP_INISIZE);
    current_directory = clone_directory(kernel_dir);
    switch_page_directory(current_directory);
}

static void tlb_flush(void)
{
    uint eflags = irq_cli();
    uint cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    if (cr0 & PAGING_ENABLED_MASK) {
        uint cr3;
        // If paging is enabled, then
        // we change the cr3 so that the tlb gets flushed
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
        __asm__ __volatile__("mov %0, %%cr3" ::"r"(cr3));
    }
    irq_sti(eflags);
}

void clear_page_entry(page_directory* pd, uint pdi, uint pti)
{
    page_entry* pe = &pd->tables_virtual[pdi]->entries[pti];
    frame_free(pe->frame << 12);
}

void clear_table_entry(page_directory* pd, uint entry)
{
    kmem_free(kernel_heap, pd->tables_virtual[entry]);
    pd->tables_virtual[entry] = NULL;
}

// Maps a page, possibly obtaining kernel space
// for a page table. We can use it freely even
// before having a kernel heap because we pre-allocate (that is what
// the kernel panic guard is for if the heap is not loaded)
void map_page(page_directory* pd, uint va, uint pa, uint flags)
{
    // We do not check that va is page-aligned.
    // This is useful because if we want to map an
    // address offset within the page, we
    // actually have to map the whole page, done
    uint pdi = PAGE_DIR(va), pti = PAGE_TABLE(va);
    if (!pd->tables_virtual[pdi]) {
        if (!kernel_heap) {
            kernel_panic("The kernel heap is not active");
        }
        void* page = kmem_alloc_aligned(kernel_heap, PAGE_SZ);
        if (page == NULL) {
            kernel_panic("No more memory for tables\n");
        }
        uint frame = physical_address(kernel_dir, (intptr)page);
        memset(page, 0, PAGE_SZ);
        page_table_entry tentry;
        memset(&tentry, 0, sizeof(tentry));
        tentry.frame = frame >> 12;
        tentry.present = 1;
        tentry.read_write = 1;
        // Table-level paging does not make sense
        // considering that it is enforced at the page level
        tentry.user = 1;
        pd->tables_phys[pdi] = tentry;
        pd->tables_virtual[pdi] = page;
    }
    page_entry* p = &pd->tables_virtual[pdi]->entries[pti];
    p->frame = pa >> PAGE_BW;
    p->present = (flags & PAGEF_P) ? 1 : 0;
    p->read_write = (flags & PAGEF_RW) ? 1 : 0,
    p->user = (flags & PAGEF_U) ? 1 : 0;
    tlb_flush(); // We flush the entire TLB (inefficient but correct)
}

// Free an entire page directory
void page_directory_destroy(page_directory* p)
{
    for (uint pdi = 0; pdi < PAGE_TABLES; pdi++) {
        page_table* pt = p->tables_virtual[pdi];
        if (!pt || pt == kernel_dir->tables_virtual[pdi])
            continue;
        for (uint pti = 0; pti < PAGE_FRAMES; pti++) {
            page_entry* pe = &pt->entries[pti];
            if (!pe->present)
                continue;

            frame_free(pe->frame << 12);
        }
        clear_table_entry(p, pdi);
    }
    kmem_free(kernel_heap, p);
}

static void page_error_kill(const char* message, exception_trace* t)
{
    scrn_cls();
    scrn_setcursor(0, 0);
    uint errorc = t->error_code;
    uint page = t->ctrace.cr2;
    uint was_present = errorc & 0x1;
    uint was_write = errorc & 0x2;
    uint was_user = errorc & 0x4;
    uint overwrite = errorc & 0x8;
    scrn_printf("PAGE FAULT: %s\n", message);
    scrn_printf("\tPage: %u Error code: %u\n", page, t->error_code);
    scrn_printf("\tPage present? %b\n", was_present);
    scrn_printf("\tOn write? %b\n", was_write);
    scrn_printf("\tUser mode? %b\n", was_user);
    scrn_printf("\tOverwrite? %b\n", overwrite);
    scrn_printf("\tTSS Selector: %u\n\n", get_tr());
    scrn_printf("EIP = %u, EFLAGS = %u, UESP = %u ESP = %u\n", t->itrace.eip,
                t->itrace.eflags, t->itrace.useresp, t->rtrace.esp);
    while (1) {
        ;
    }
}

void do_copy_on_write(uint page, exception_trace* t)
{
    uint pdi = PAGE_DIR(page), pti = PAGE_TABLE(page);
    page_entry* p = &current_directory->tables_virtual[pdi]->entries[pti];
    if (!p->copy_on_write) {
        page_error_kill("WRITE ON READ-ONLY PAGE", t);
    }
    uint original = p->frame << 12;
    uint new = frame_alloc();
    copy_frame(new, original);
    p->frame = new >> 12;
    p->copy_on_write = 0;
    p->read_write = 1;
}

void page_fault_handler(exception_trace t)
{
    // We do not want to be interrupted: This modifies kernel
    // pages and such things.
    uint eflags = irq_cli();
    uint errorc = t.error_code;
    uint page = t.ctrace.cr2;
    uint was_present = errorc & 0x1;
    uint was_write = errorc & 0x2;
    uint was_user = errorc & 0x4;
    uint overwrite = errorc & 0x8;
    if (was_present && was_write && !overwrite) {
        do_copy_on_write(page, &t);
    } else {
        if (was_user) {
            if (overwrite) {
                page_error_kill("CONTROL BITS OVERWRITTEN", &t);
            }
            if (!was_present) {
                // TODO: Look into doing swapping to disk.
                page_error_kill("PAGE NOT PRESENT", &t);
            } else {
                page_error_kill("PROTECTION ERROR IN PAGING\n", &t);
            }
            // do_exit();
        } else {
            // TODO: This is the only case in which
            // the kernel must be killed. In the
            // others the process must be killed.
            page_error_kill("ERROR IN THE KERNEL", &t);
            while (1)
                ;
        }
    }
    irq_sti(eflags);
}
