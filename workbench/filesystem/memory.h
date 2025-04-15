#ifndef __MEMORY_H
#define __MEMORY_H

#include "types.h"

void kmem_init(void *, uint);
void * kmem_alloc(uint);
void kmem_free(void*);
void * kmem_alloc_aligned(uint);

#endif
