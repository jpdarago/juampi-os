#ifndef __MEMORY_H
#define __MEMORY_H

#include <alloc.h>

#include <stdint.h>
#include <stddef.h>

// A segregated, mimalloc-inspired kernel heap exposed through the alloc.h
// interface. Small allocations are served from per-size-class free lists carved
// out of 64 KiB slabs: O(1) alloc and free, and no per-object header — the
// owning slab (and thus the size class) is recovered from a pointer by masking
// it down to its slab and indexing a descriptor array. Large allocations take a
// run of whole slabs, tracked on a free-run list (the "free list of big
// blocks"). The heap is a value type so it can later become per-CPU (the
// mimalloc "thread-local heap"); today there is one, single-threaded.

#define HEAP_NCLASSES 18

typedef struct slab slab; // per-slab descriptor, defined in memory.c

typedef struct {
    allocator base;  // must be first: a heap_allocator* is an allocator*
    char* base_addr; // first slab (64 KiB-aligned)
    char* bump;      // next un-carved slab
    char* end;       // end of the slab region
    slab* descs;     // per-slab descriptor array
    slab* partial[HEAP_NCLASSES]; // slabs with a free block, per size class
    slab* free_runs;              // free list of large (multi-slab) runs
} heap_allocator;

// Build a heap over [beg, beg + size); beg should be 64 KiB-aligned.
heap_allocator heap_init(void* beg, ptrdiff_t size);
// Return a block previously handed out by this heap's alloc().
void heap_free(heap_allocator* h, void* p);
// Usable size of a block (its size class, or the run length for large blocks).
size_t heap_usable_size(heap_allocator* h, void* p);

// The default kernel heap, used by the libc shim's malloc/free/realloc. Set
// once at boot from kmain.
void heap_set_default(heap_allocator* h);
heap_allocator* heap_default(void);

#endif
