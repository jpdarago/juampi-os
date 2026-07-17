#include <bitset.h>
#include <utils.h>

#define DWORD_SZ 32

// This function initializes only the offset
// parameters of a bitset. It is because it is used for minix
// where we do not want to clear the final chunk of the bitmap
// because minix already gives it to us correctly
uint32* bitset_load(bitset* b, void* start, uint size)
{
    b->start = start;
    b->size = CEIL(size, DWORD_SZ);
    return (uint32*)(b->start + b->size);
}

// Initializes the bitset so that it uses the bitmap
// loaded in the chunk of memory that starts at start.
// The size is the number of things to manage
uint32* bitset_init(bitset* b, void* start, uint size)
{
    uint32* res = bitset_load(b, start, size);
    memset(start, 0, size / 8);
    if (size % DWORD_SZ)
        b->start[size / DWORD_SZ] = ~((1 << (size % DWORD_SZ)) - 1);
    return res;
}

void bitset_set(bitset* b, uint index)
{
    if (index >= DWORD_SZ * b->size) {
        return;
    }
    b->start[index / DWORD_SZ] |= 1 << (index % DWORD_SZ);
}

void bitset_clear(bitset* b, uint index)
{
    if (index >= DWORD_SZ * b->size) {
        return;
    }
    b->start[index / DWORD_SZ] &= ~(1 << (index % DWORD_SZ));
}

// Returns the index of the first clear bit, or (uint)-1 if the set is full.
// (Ported from the old 32-bit bitset_search.asm to portable C.)
uint bitset_search(bitset* b)
{
    for (uint32 i = 0; i < b->size; i++) {
        uint32 word = b->start[i];
        if (word != 0xFFFFFFFF) {
            for (uint bit = 0; bit < DWORD_SZ; bit++) {
                if (!(word & (1u << bit))) {
                    return i * DWORD_SZ + bit;
                }
            }
        }
    }
    return (uint)-1;
}
