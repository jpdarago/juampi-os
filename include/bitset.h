#ifndef __BITSET_H
#define __BITSET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint32_t* start;
    uint32_t size;
} bitset;

// Initializes the bitset: the size is the number of things to manage.
// Returns the memory address where the bitmap ends.
uint32_t* bitset_init(bitset* b, void* start, uint32_t size);
// Marks the bit as used.
void bitset_set(bitset* b, uint32_t index);
// Marks the bit as free.
void bitset_clear(bitset* b, uint32_t index);
// Returns the index of a free bit in the bitmap, or (uint32_t)-1 if full.
uint32_t bitset_search(bitset* b);

#endif
