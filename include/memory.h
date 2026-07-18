#ifndef __MEMORY_H
#define __MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct kmem_header {
    struct kmem_header* next;
    int size;
} kmem_header;

typedef struct {
    kmem_header* freep;
    uintptr_t heap_end;
} kmem_map_header;

kmem_map_header* kmem_init(void*, int);
void* kmem_alloc(kmem_map_header*, int);
void kmem_free(kmem_map_header*, void*);
void* kmem_alloc_aligned(kmem_map_header*, int);

// Gets kernel memory
void* kmalloc(uint32_t size);
// Returns memory to the kernel heap
void kfree(void* mem);

#endif
