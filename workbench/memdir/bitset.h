#ifndef __BITSET_H
#define __BITSET_H

#include "types.h"

typedef struct {
	uint * start;
	uint size;
} bitset;

void bitset_init(bitset * b, void * start, uint size);
void bitset_set(bitset * b, uint index);
void bitset_clear(bitset * b, uint index);
uint bitset_search(bitset * b);

#endif
