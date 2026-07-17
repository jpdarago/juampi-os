// x86-64 paging over the Limine higher-half direct map (HHDM). Limine hands us
// a long-mode environment where all physical RAM is mapped at hhdm_offset + pa,
// so page tables are edited directly through that window — no software shadow
// of the tables and no temporary mappings are needed. Fork/copy-on-write, the
// page fault handler and address-space teardown return in later milestones
// (they depend on the not-yet-ported task and exception subsystems).

#include <paging.h>
#include <frames.h>
#include <memory.h>
#include <panic.h>
#include <utils.h>

// Hardware page-table entry bits.
#define PTE_P (1ull << 0)
#define PTE_RW (1ull << 1)
#define PTE_US (1ull << 2)
#define PTE_PS (1ull << 7)
#define PTE_ADDR 0x000ffffffffff000ull

// Kernel heap: a private higher-half window (PML4 slot 384, clear of Limine's
// HHDM at slot 256 and the kernel image at slot 511), grown a page at a time by
// mapping fresh frames into it.
#define KHEAP_START 0xffffc00000000000ull
#define KHEAP_INISIZE 0x400000ull           // 4 MiB
#define KHEAP_MAXIMUM 0xffffc00040000000ull // 1 GiB window

uintptr hhdm_offset;
page_directory *current_directory, *kernel_dir;
static page_directory kernel_directory;
static kmem_map_header* kernel_heap;

kmem_map_header* get_kernel_heap(void)
{
    return kernel_heap;
}

// Return the table one level down from t->entries[idx], allocating and linking
// a fresh zeroed table (present, writable, user) if the entry is empty.
static page_table* level_next(page_table* t, uint idx)
{
    pte_t e = t->entries[idx];
    if (!(e & PTE_P)) {
        uintptr frame = frame_alloc();
        memset(phys_to_virt(frame), 0, PAGE_SZ);
        t->entries[idx] = (frame & PTE_ADDR) | PTE_P | PTE_RW | PTE_US;
        return (page_table*)phys_to_virt(frame);
    }
    return (page_table*)phys_to_virt(e & PTE_ADDR);
}

void map_page(page_directory* pd, uintptr va, uintptr pa, uint flags)
{
    page_table* pml4 = (page_table*)phys_to_virt(pd->pml4_phys);
    page_table* pdpt = level_next(pml4, PML4_INDEX(va));
    page_table* pd_t = level_next(pdpt, PDPT_INDEX(va));
    page_table* pt = level_next(pd_t, PD_INDEX(va));

    pte_t e = pa & PTE_ADDR;
    if (flags & PAGEF_P)
        e |= PTE_P;
    if (flags & PAGEF_RW)
        e |= PTE_RW;
    if (flags & PAGEF_U)
        e |= PTE_US;
    pt->entries[PT_INDEX(va)] = e;
    __asm__ __volatile__("invlpg (%0)" ::"r"(va) : "memory");
}

uintptr physical_address(page_directory* pd, uintptr va)
{
    page_table* t = (page_table*)phys_to_virt(pd->pml4_phys);
    pte_t e = t->entries[PML4_INDEX(va)];
    if (!(e & PTE_P))
        return (uintptr)-1;
    t = (page_table*)phys_to_virt(e & PTE_ADDR);
    e = t->entries[PDPT_INDEX(va)];
    if (!(e & PTE_P))
        return (uintptr)-1;
    if (e & PTE_PS)
        return (e & PTE_ADDR) + (va & 0x3FFFFFFF); // 1 GiB page
    t = (page_table*)phys_to_virt(e & PTE_ADDR);
    e = t->entries[PD_INDEX(va)];
    if (!(e & PTE_P))
        return (uintptr)-1;
    if (e & PTE_PS)
        return (e & PTE_ADDR) + (va & 0x1FFFFF); // 2 MiB page
    t = (page_table*)phys_to_virt(e & PTE_ADDR);
    e = t->entries[PT_INDEX(va)];
    if (!(e & PTE_P))
        return (uintptr)-1;
    return (e & PTE_ADDR) + PAGE_OFFSET(va);
}

// Walk the current address space and check that the page containing `va` is
// present and user-accessible (and writable, if requested), honouring large
// pages. This is the per-page primitive behind user_access_ok.
static bool page_user_ok(uintptr va, bool write)
{
    uint idx[4] = {PML4_INDEX(va), PDPT_INDEX(va), PD_INDEX(va), PT_INDEX(va)};
    page_table* t = (page_table*)phys_to_virt(current_directory->pml4_phys);
    for (int lvl = 0; lvl < 4; lvl++) {
        pte_t e = t->entries[idx[lvl]];
        if (!(e & PTE_P) || !(e & PTE_US))
            return false;
        bool leaf = (lvl == 3) || (e & PTE_PS);
        if (leaf)
            return !write || (e & PTE_RW);
        t = (page_table*)phys_to_virt(e & PTE_ADDR);
    }
    return false;
}

// Checks that [addr, addr+len) is entirely mapped in the current directory with
// the user bit (and, for writes, the read/write bit) set. This is the gate for
// any pointer that crosses the syscall boundary: it rejects kernel addresses
// and unmapped pages so a user program cannot make the kernel read or write
// outside its own address space.
bool user_access_ok(uintptr addr, uintptr len, bool write)
{
    if (len == 0)
        return true;
    if (addr + len < addr) // wraparound
        return false;
    uintptr last_page = (addr + len - 1) & ~0xFFFull;
    for (uintptr page = addr & ~0xFFFull;; page += PAGE_SZ) {
        if (!page_user_ok(page, write))
            return false;
        if (page >= last_page)
            break;
    }
    return true;
}

// Checks that a NUL-terminated user string is entirely readable in user space,
// scanning at most `max` bytes (including the terminator).
bool user_string_ok(const char* s, uint max)
{
    uintptr addr = (uintptr)s;
    for (uint i = 0; i < max; i++) {
        if (i == 0 || ((addr + i) & 0xFFF) == 0) {
            if (!user_access_ok(addr + i, 1, false))
                return false;
        }
        if (((const char*)addr)[i] == '\0')
            return true;
    }
    return false;
}

void* paging_append_core(kmem_map_header* mh, uint pages)
{
    void* prev = (void*)mh->heap_end;
    while (pages-- > 0) {
        if ((uintptr)mh->heap_end >= KHEAP_MAXIMUM)
            kernel_panic("Heap size exceeded");
        uintptr frame = frame_alloc();
        map_page(kernel_dir, mh->heap_end, frame, PAGEF_P | PAGEF_RW);
        mh->heap_end += PAGE_SZ;
    }
    return prev;
}

void paging_init(uintptr hhdm, uintptr usable_phys_base, uintptr usable_len)
{
    hhdm_offset = hhdm;

    // Adopt the page tables Limine already built (kernel image + HHDM mapped).
    uintptr cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    kernel_directory.pml4_phys = cr3 & PTE_ADDR;
    kernel_dir = &kernel_directory;
    current_directory = &kernel_directory;

    // Frame allocator over the usable physical region Limine reported.
    frames_init(usable_phys_base, usable_len);

    // Back the initial kernel heap with fresh frames, then hand the region to
    // the K&R allocator.
    for (uintptr p = KHEAP_START; p < KHEAP_START + KHEAP_INISIZE;
         p += PAGE_SZ) {
        map_page(kernel_dir, p, frame_alloc(), PAGEF_P | PAGEF_RW);
    }
    kernel_heap = kmem_init((void*)KHEAP_START, KHEAP_INISIZE);
}
