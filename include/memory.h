#ifndef __MEMORY_H
#define __MEMORY_H

#include <alloc.h>

#include <stdint.h>

// The kernel heap: a K&R-style best-fit free-list allocator exposed through
// the generic alloc.h interface. Unlike the arena it supports returning
// individual blocks with heap_free(), so it backs long-lived allocations whose
// lifetimes do not nest.

typedef struct kmem_header {
    struct kmem_header* next;
    ptrdiff_t size;
} kmem_header;

typedef struct {
    allocator base; // must be first: a heap_allocator* is an allocator*
    kmem_header* freep;
    char* heap_end;
} heap_allocator;

// Create a heap over [beg, beg + size).
heap_allocator heap_init(void* beg, ptrdiff_t size);
// Return a block previously handed out by this heap's alloc().
void heap_free(heap_allocator* h, void* p);

#endif
