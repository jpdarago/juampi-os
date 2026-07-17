#ifndef __BITSET_H
#define __BITSET_H

#include <types.h>

typedef struct {
    uint32* start;
    uint32 size;
} bitset;

// Initializes the bitset: The size is the
// number of things to manage. Returns the
// memory address where the bitset ends
uint32* bitset_init(bitset* b, void* start, uint size);
// Loads a bitset given already existing memory (for MINIX for example)
uint32* bitset_load(bitset*, void*, uint);
// Marks the bit as used
void bitset_set(bitset* b, uint index);
// Marks the bit as free
void bitset_clear(bitset* b, uint index);
// Returns the index of a free bit in
// the bitmap. Due to its importance
// bitset_search is written in assembler.
uint bitset_search(bitset* b);

#endif
