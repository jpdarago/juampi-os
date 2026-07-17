#ifndef __PAGING_H
#define __PAGING_H

#include <types.h>
#include <exception.h>
#include <memory.h>

#define PAGE_SZ 0x1000
#define PAGE_TABLES 1024
#define PAGE_FRAMES 1024

#define PAGING_ENABLED_MASK (1 << 31)
#define WP_MASK (1 << 16)

typedef struct {
    uint32 present : 1;
    uint32 read_write : 1;
    uint32 user : 1;
    uint32 write_through : 1;
    uint32 cache_disabled : 1;
    uint32 accessed : 1;
    uint32 dirty : 1;
    uint32 __zero_1 : 1;
    uint32 global : 1;
    // Copy on write: If a page has this bit set
    // and someone tries to write to it when it is in read-only
    // mode, a copy must be made and reassigned. It is part
    // of Intel's available bits.
    uint32 copy_on_write : 1;
    uint32 avail : 2;
    uint32 frame : 20;
} page_entry;

#define PAGEF_P 1
#define PAGEF_RW 2
#define PAGEF_U 4
#define PAGEF_A 8
#define PAGEF_FULL (PAGEF_P | PAGEF_RW | PAGEF_U)

typedef struct {
    uint32 present : 1;
    uint32 read_write : 1;
    uint32 user : 1;
    uint32 write_through : 1;
    uint32 cache_disabled : 1;
    uint32 accessed : 1;
    uint32 __zero : 1;
    uint32 size : 1;
    uint32 global : 1;
    uint32 avail : 3;
    uint32 frame : 20;
} page_table_entry;

typedef struct {
    page_entry entries[1024];
} page_table;

typedef struct {
    // The entries (which contain physical addresses)
    // of the tables of this directory. This is for when
    // we have to clone the entries (when forking the process).
    page_table_entry tables_phys[1024];
    // The addresses (in virtual memory space)
    // corresponding to the contents of the directory
    // tables
    page_table* tables_virtual[1024];
    // Physical address where the directory entries
    // (the tables) begin. This goes in the kernel's cr3.
    uint32 physical_address;
} page_directory;

#define PAGE_DIR(x) ((x) >> 22)
#define PAGE_TABLE(x) (((x) & 0x3FF000) >> 12)
#define PAGE_OFFSET(x) ((x) & 0xFFF)

#define ALIGN(x) ((x) & ~0xFFF)
#define NEXT_ALIGN(x) ALIGN((x) + 0x1000)

// Returns the physical address of the virtual address in
// the directory, both passed as parameters
uint32 physical_address(page_directory*, uint);
// Initializes paging by doing identity mapping and creating
// the exception handlers and necessary structures
void paging_init(intptr end_address, intptr kernel_last_addr);
// Maps a virtual address to a physical one in a directory given
// the desired flags
void map_page(page_directory* pd, uint va, uint pa, uint flags);
// Gets more memory for the indicated memory map
void* paging_append_core(kmem_map_header*, uint);
// Changes the page directory to the one passed as parameter
void switch_page_directory(page_directory* pd);
void page_fault_handler(exception_trace);
// Clones a directory doing copy on write
page_directory* clone_directory(page_directory*);
// Current page directory and kernel page directory
// (the second one is to know which things belong to the kernel)
extern page_directory *current_directory, *kernel_dir;
// Copies two frames: It disables paging to do so and that is
// why it is written in assembler in copy_frame.asm
extern void copy_frame(uint dst, uint src);
// Changes the current directory to another one
void set_current_directory(page_directory*);
// Returns the address of the kernel heap
kmem_map_header* get_kernel_heap(void);
// Clears a page or table entry from the current directory
void clear_page_entry(page_directory* pe, uint, uint);
void clear_table_entry(page_directory* pe, uint);
// Delete a whole page directory
void page_directory_destroy(page_directory*);
// Validate that a user-supplied pointer/range or string lies in the current
// process's user-accessible address space (used to guard the syscall boundary).
bool user_access_ok(uint addr, uint len, bool write);
bool user_string_ok(const char* s, uint max);
#endif
