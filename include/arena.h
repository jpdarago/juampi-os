#ifndef __ARENA_H
#define __ARENA_H

#include <alloc.h>

// Arena (linear) allocator implementing the alloc.h interface. Allocation bumps
// `beg` with alignment padding; there is no per-object free — lifetimes are
// managed by copying the arena struct (scoped scratch) or resetting it
// wholesale. Useful for transient work with a clear lifetime; long-lived
// allocations with individual frees belong to the kernel heap (memory.h).
typedef struct {
    allocator base; // must be first: an arena* is an allocator*
    char* beg;
    char* end;
} arena;

// Create an arena over [beg, beg + size).
arena arena_init(void* beg, ptrdiff_t size);

#endif
