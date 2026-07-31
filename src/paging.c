// x86-64 paging over the Limine higher-half direct map (HHDM). Limine hands us
// a long-mode environment where all physical RAM is mapped at hhdm_offset + pa,
// so page tables are edited directly through that window — no software shadow
// of the tables and no temporary mappings are needed. Fork/copy-on-write, the
// page fault handler and address-space teardown return in later milestones
// (they depend on the not-yet-ported task and exception subsystems).

#include <paging.h>
#include <frames.h>
#include <panic.h>
#include <utils.h>

// Hardware page-table entry bits.
#define PTE_P (1ull << 0)
#define PTE_RW (1ull << 1)
#define PTE_US (1ull << 2)
#define PTE_PS (1ull << 7)
#define PTE_PWT (1ull << 3)
#define PTE_PCD (1ull << 4)
#define PTE_ADDR 0x000ffffffffff000ull

// Kernel heap window: a private higher-half range (PML4 slot 384, clear of
// Limine's HHDM at slot 256 and the kernel image at slot 511), backed with
// fresh frames at init and handed to the caller to build allocators over.
#define KHEAP_START 0xffffc00000000000ull

uintptr_t hhdm_offset;
struct page_directory *current_directory, *kernel_dir;
static struct page_directory kernel_directory;

// Return the table one level down from t->entries[idx], allocating and linking
// a fresh zeroed table (present, writable, user) if the entry is empty.
static struct page_table* level_next(struct page_table* t, uint32_t idx)
{
    pte_t e = t->entries[idx];
    if (!(e & PTE_P)) {
        uintptr_t frame = frame_alloc();
        memset(phys_to_virt(frame), 0, PAGE_SZ);
        t->entries[idx] = (frame & PTE_ADDR) | PTE_P | PTE_RW | PTE_US;
        return (struct page_table*)phys_to_virt(frame);
    }
    return (struct page_table*)phys_to_virt(e & PTE_ADDR);
}

void map_page(struct page_directory* pd, uintptr_t va, uintptr_t pa,
              uint32_t flags)
{
    struct page_table* pml4 = (struct page_table*)phys_to_virt(pd->pml4_phys);
    struct page_table* pdpt = level_next(pml4, PML4_INDEX(va));
    struct page_table* pd_t = level_next(pdpt, PDPT_INDEX(va));
    struct page_table* pt = level_next(pd_t, PD_INDEX(va));

    pte_t e = pa & PTE_ADDR;
    if (flags & PAGEF_P)
        e |= PTE_P;
    if (flags & PAGEF_RW)
        e |= PTE_RW;
    if (flags & PAGEF_U)
        e |= PTE_US;
    if (flags & PAGEF_UC)
        e |= PTE_PCD | PTE_PWT; // strongly-ordered uncached, for device MMIO
    pt->entries[PT_INDEX(va)] = e;
    __asm__ __volatile__("invlpg (%0)" ::"r"(va) : "memory");
}

// Device-MMIO virtual windows are bump-allocated from a dedicated higher-half
// region (PML4 slot 448, clear of the HHDM, the kernel heap at slot 384, and
// the kernel image), so drivers never hand-pick — and can't collide on — VA
// windows.
#define IOMAP_BASE 0xffffe00000000000ull
#define IOMAP_END 0xffffe00040000000ull // 1 GiB of device MMIO space
static uintptr_t iomap_next = IOMAP_BASE;

void* iomap(uintptr_t pa, size_t len, uint32_t flags)
{
    uintptr_t off = pa & (PAGE_SZ - 1); // preserve any sub-page BAR offset
    uintptr_t base = pa - off;
    size_t pages = (len + off + PAGE_SZ - 1) / PAGE_SZ;
    uintptr_t va = iomap_next;
    if (va + pages * PAGE_SZ > IOMAP_END) {
        kernel_panic("iomap: device MMIO window space exhausted");
    }
    for (size_t i = 0; i < pages; i++) {
        map_page(kernel_dir, va + i * PAGE_SZ, base + i * PAGE_SZ, flags);
    }
    iomap_next = va + pages * PAGE_SZ;
    return (void*)(va + off);
}

uintptr_t physical_address(struct page_directory* pd, uintptr_t va)
{
    struct page_table* t = (struct page_table*)phys_to_virt(pd->pml4_phys);
    pte_t e = t->entries[PML4_INDEX(va)];
    if (!(e & PTE_P))
        return (uintptr_t)-1;
    t = (struct page_table*)phys_to_virt(e & PTE_ADDR);
    e = t->entries[PDPT_INDEX(va)];
    if (!(e & PTE_P))
        return (uintptr_t)-1;
    if (e & PTE_PS)
        return (e & PTE_ADDR) + (va & 0x3FFFFFFF); // 1 GiB page
    t = (struct page_table*)phys_to_virt(e & PTE_ADDR);
    e = t->entries[PD_INDEX(va)];
    if (!(e & PTE_P))
        return (uintptr_t)-1;
    if (e & PTE_PS)
        return (e & PTE_ADDR) + (va & 0x1FFFFF); // 2 MiB page
    t = (struct page_table*)phys_to_virt(e & PTE_ADDR);
    e = t->entries[PT_INDEX(va)];
    if (!(e & PTE_P))
        return (uintptr_t)-1;
    return (e & PTE_ADDR) + PAGE_OFFSET(va);
}

void* paging_init(uintptr_t hhdm, uintptr_t usable_phys_base,
                  uintptr_t usable_len)
{
    hhdm_offset = hhdm;

    // Adopt the page tables Limine already built (kernel image + HHDM mapped).
    uintptr_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    kernel_directory.pml4_phys = cr3 & PTE_ADDR;
    kernel_dir = &kernel_directory;
    current_directory = &kernel_directory;

    // Frame allocator over the usable physical region Limine reported.
    frames_init(usable_phys_base, usable_len);

    // Back the kernel-heap window with fresh frames; the caller builds its
    // allocators over the returned region.
    for (uintptr_t p = KHEAP_START; p < KHEAP_START + KHEAP_SIZE;
         p += PAGE_SZ) {
        map_page(kernel_dir, p, frame_alloc(), PAGEF_P | PAGEF_RW);
    }
    return (void*)KHEAP_START;
}
