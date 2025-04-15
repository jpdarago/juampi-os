#include "bitset.h"
#include <string.h>

#define CEIL(a,b) ((a)+(b)-1)/(b)
#define DWORD_SZ 32

uint bitset_load(bitset * b, void * start, uint size)
{
	b->start = start;
	b->size = CEIL(size,DWORD_SZ);
	return (uint)(b->start + b->size);
}

uint bitset_init(bitset * b, void * start, uint size)
{
	uint res = bitset_load(b,start,size);
	memset(start,0,size/8);
	if(size % DWORD_SZ)
		b->start[size/DWORD_SZ] =
		    ~((1 << (size % DWORD_SZ))-1);
	return res;
}

void bitset_set(bitset * b, uint index)
{
	if(index >= DWORD_SZ*b->size)
		return;

	b->start[index/DWORD_SZ] |= 1 << (index % DWORD_SZ);
}

void bitset_clear(bitset * b, uint index)
{
	if(index >= DWORD_SZ*b->size)
		return;

	b->start[index/DWORD_SZ] &= ~(1 << (index % DWORD_SZ));
}

int bitset_get(bitset * b, uint index)
{
	if(index >= DWORD_SZ*b->size)
		return -1;
	return b->start[index/DWORD_SZ] & (1 << (index % DWORD_SZ));
}
