#include <bitset.h>
#include <utils.h>

#define DWORD_SZ 32

// Initializes the bitset so that it uses the bitmap loaded in the chunk of
// memory that starts at start. The size is the number of things to manage; the
// bits past `size` in the final word are marked used so a search never returns
// them. Returns the address just past the bitmap.
uint32_t* bitset_init(bitset* b, void* start, uint32_t size)
{
    b->start = start;
    b->size = CEIL(size, DWORD_SZ);
    uint32_t* res = (uint32_t*)(b->start + b->size);
    memset(start, 0, size / 8);
    if (size % DWORD_SZ)
        b->start[size / DWORD_SZ] = ~((1 << (size % DWORD_SZ)) - 1);
    return res;
}

void bitset_set(bitset* b, uint32_t index)
{
    if (index >= DWORD_SZ * b->size) {
        return;
    }
    b->start[index / DWORD_SZ] |= 1 << (index % DWORD_SZ);
}

void bitset_clear(bitset* b, uint32_t index)
{
    if (index >= DWORD_SZ * b->size) {
        return;
    }
    b->start[index / DWORD_SZ] &= ~(1 << (index % DWORD_SZ));
}

// Returns the index of the first clear bit, or (uint32_t)-1 if the set is full.
// (Ported from the old 32-bit bitset_search.asm to portable C.)
uint32_t bitset_search(bitset* b)
{
    for (uint32_t i = 0; i < b->size; i++) {
        uint32_t word = b->start[i];
        if (word != 0xFFFFFFFF) {
            for (uint32_t bit = 0; bit < DWORD_SZ; bit++) {
                if (!(word & (1u << bit))) {
                    return i * DWORD_SZ + bit;
                }
            }
        }
    }
    return (uint32_t)-1;
}
