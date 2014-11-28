#include "bitset.h"
#include <string.h>

#define DWORD_BW 5
#define DWORD_SZ (1 << DWORD_BW)

void bitset_init(bitset * b, void * start, unsigned int dword_size)
{
	b->start = start; b->size = dword_size;
	memset(start,0,dword_size*sizeof(int));
}

void bitset_set(bitset * b, unsigned int index)
{
	if((index >> DWORD_BW) >= b->size)
		return;
	
	b->start[index >> DWORD_BW] 
		|= 1 << (index & (DWORD_SZ-1));
}

void bitset_clear(bitset * b, unsigned int index)
{
	if((index >> DWORD_BW) >= b->size)
		return;
	
	b->start[index >> DWORD_BW] 
		&= ~(1 << (index & (DWORD_SZ-1)));
}
