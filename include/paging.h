#ifndef __PAGING_H
#define __PAGING_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PAGE_SZ 0x1000
#define PAGE_BW 12

// Page-table entry flags, as passed to map_page (kept from the 32-bit ABI so
// callers do not change). The hardware bit layout is applied inside paging.c.
#define PAGEF_P 1  // present
#define PAGEF_RW 2 // writable
#define PAGEF_U 4  // user-accessible
#define PAGEF_UC 8 // uncacheable (PCD|PWT) — for device MMIO (e.g. a NIC BAR)

// x86-64 uses 4-level paging: PML4 -> PDPT -> PD -> PT, each a 512-entry table
// of 8-byte entries. One entry type covers every level.
typedef uint64_t pte_t;

struct page_table {
    pte_t entries[512];
};

// A virtual address space, identified by the physical address of its PML4
// (what goes in CR3). Under the Limine higher-half direct map (HHDM) every
// physical page — including the page tables — is reachable at hhdm_offset + pa,
// so no software shadow of the tables is needed.
struct page_directory {
    uintptr_t pml4_phys;
};

// Index of a virtual address into each paging level.
#define PML4_INDEX(x) (((x) >> 39) & 0x1FF)
#define PDPT_INDEX(x) (((x) >> 30) & 0x1FF)
#define PD_INDEX(x) (((x) >> 21) & 0x1FF)
#define PT_INDEX(x) (((x) >> 12) & 0x1FF)
#define PAGE_OFFSET(x) ((x) & 0xFFF)

extern struct page_directory *current_directory, *kernel_dir;

// The Limine higher-half direct map offset: virtual = hhdm_offset + physical
// for all of RAM. Set once at boot.
extern uintptr_t hhdm_offset;
static inline void* phys_to_virt(uintptr_t pa)
{
    return (void*)(hhdm_offset + pa);
}

// Size of the kernel-heap window paging_init maps. Grown over time to hold, out
// of this one window: the per-core Lua heaps (parallel.h / M9 — 8 MiB each),
// the filesystem's private 16 MiB scratch heap (ext2.c), and a hosted program's
// 64 MiB heap (syscall.c) while it runs. Still a fraction of usable RAM (QEMU
// gives 512 MiB; the XPS far more), and there is 16 TiB of VA headroom above
// KHEAP_START.
#define KHEAP_SIZE 0x10000000ull // 256 MiB

// Bring up the memory subsystem on top of what Limine set up: adopt its page
// tables, record the HHDM offset, initialise the frame allocator over the
// given usable physical region, and map the kernel-heap window. Returns the
// start of that window (KHEAP_SIZE bytes) for the caller to build allocators
// over. Called once from kmain.
void* paging_init(uintptr_t hhdm, uintptr_t usable_phys_base,
                  uintptr_t usable_len);

// Map va -> pa in the given address space with the given PAGEF_* flags,
// allocating intermediate tables from the frame allocator as needed.
void map_page(struct page_directory* pd, uintptr_t va, uintptr_t pa,
              uint32_t flags);

// Reserve a fresh device-MMIO window: map `len` bytes of physical memory
// starting at `pa` (page-aligned; the sub-page offset is preserved) into a
// bump-allocated higher-half virtual region with the given PAGEF_* flags, and
// return the usable pointer. Drivers use this instead of hand-picking VA
// windows, so device MMIO can't collide (a NIC BAR + PAGEF_UC, the
// LAPIC/IOAPIC, the framebuffer aperture cached, ...).
void* iomap(uintptr_t pa, size_t len, uint32_t flags);
// Physical address backing va, or (uintptr_t)-1 if unmapped.
uintptr_t physical_address(struct page_directory* pd, uintptr_t va);

#endif
