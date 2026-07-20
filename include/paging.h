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

// x86-64 uses 4-level paging: PML4 -> PDPT -> PD -> PT, each a 512-entry table
// of 8-byte entries. One entry type covers every level.
typedef uint64_t pte_t;

typedef struct {
    pte_t entries[512];
} page_table;

// A virtual address space, identified by the physical address of its PML4
// (what goes in CR3). Under the Limine higher-half direct map (HHDM) every
// physical page — including the page tables — is reachable at hhdm_offset + pa,
// so no software shadow of the tables is needed.
typedef struct page_directory {
    uintptr_t pml4_phys;
} page_directory;

// Index of a virtual address into each paging level.
#define PML4_INDEX(x) (((x) >> 39) & 0x1FF)
#define PDPT_INDEX(x) (((x) >> 30) & 0x1FF)
#define PD_INDEX(x) (((x) >> 21) & 0x1FF)
#define PT_INDEX(x) (((x) >> 12) & 0x1FF)
#define PAGE_OFFSET(x) ((x) & 0xFFF)

extern page_directory *current_directory, *kernel_dir;

// The Limine higher-half direct map offset: virtual = hhdm_offset + physical
// for all of RAM. Set once at boot.
extern uintptr_t hhdm_offset;
static inline void* phys_to_virt(uintptr_t pa)
{
    return (void*)(hhdm_offset + pa);
}

// Size of the kernel-heap window paging_init maps. Enlarged from 16 MiB to make
// room for the per-core Lua heaps (parallel.h / M9): each worker interpreter
// carves an 8 MiB private heap out of this window. Still a fraction of usable
// RAM, and there is 16 TiB of VA headroom above KHEAP_START.
#define KHEAP_SIZE 0x8000000ull // 128 MiB

// Bring up the memory subsystem on top of what Limine set up: adopt its page
// tables, record the HHDM offset, initialise the frame allocator over the
// given usable physical region, and map the kernel-heap window. Returns the
// start of that window (KHEAP_SIZE bytes) for the caller to build allocators
// over. Called once from kmain.
void* paging_init(uintptr_t hhdm, uintptr_t usable_phys_base,
                  uintptr_t usable_len);

// Map va -> pa in the given address space with the given PAGEF_* flags,
// allocating intermediate tables from the frame allocator as needed.
void map_page(page_directory* pd, uintptr_t va, uintptr_t pa, uint32_t flags);
// Physical address backing va, or (uintptr_t)-1 if unmapped.
uintptr_t physical_address(page_directory* pd, uintptr_t va);

// Validate that a user-supplied pointer/range or string lies in the current
// process's user-accessible address space (used to guard the syscall boundary).
bool user_access_ok(uintptr_t addr, uintptr_t len, bool write);
bool user_string_ok(const char* s, size_t max);

#endif
