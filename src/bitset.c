#include <bitset.h>
#include <utils.h>
#include <scrn.h>

#define DWORD_SZ 32

// This function initializes only the offset
// parameters of a bitset. It is because it is used for minix
// where we do not want to clear the final chunk of the bitmap
// because minix already gives it to us correctly
uint32 * bitset_load(bitset* b, void* start, uint size)
{
    b->start = start;
    b->size = CEIL(size,DWORD_SZ);
    return (uint32 *)(b->start + b->size);
}

// Initializes the bitset so that it uses the bitmap
// loaded in the chunk of memory that starts at start.
// The size is the number of things to manage
uint32 * bitset_init(bitset* b, void* start, uint size)
{
    uint32 * res = bitset_load(b,start,size);
    memset(start,0,size/8);
    if(size % DWORD_SZ)
        b->start[size/DWORD_SZ] =
            ~((1 << (size % DWORD_SZ))-1);
    return res;
}

void bitset_set(bitset* b, uint index)
{
    if(index >= DWORD_SZ*b->size) {
        return;
    }
    b->start[index/DWORD_SZ] |= 1 << (index % DWORD_SZ);
}

void bitset_clear(bitset* b, uint index)
{
    if(index >= DWORD_SZ*b->size) {
        return;
    }
    b->start[index/DWORD_SZ] &= ~(1 << (index % DWORD_SZ));
}
