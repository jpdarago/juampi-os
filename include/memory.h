#ifndef __MEMORY_H
#define __MEMORY_H

#include <types.h>

typedef struct kmem_header {
    struct kmem_header * next;
    int size;
} kmem_header;

typedef struct {
    kmem_header * freep;
    intptr heap_end;
} kmem_map_header;

kmem_map_header * kmem_init(void *, int);
void * kmem_alloc(kmem_map_header *, int);
void kmem_free(kmem_map_header *, void *);
void * kmem_alloc_aligned(kmem_map_header *, int);

// Consigue memoria de kernel
void * kmalloc(uint size);
// Devuelve memoria a la heap de kernel
void kfree(void * mem);

#endif
