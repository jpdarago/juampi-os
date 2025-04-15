#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bitset.h"
#include <assert.h>

uint _bitset_search(bitset * b){
	for(uint i = 0;i < b->size; i++){
		uint v = b->start[i];
		if(v != 0xFFFFFFFF){
			for(uint bi = 0; bi < 32; bi++){
				if(!(v & (1 << bi)))
						return 32*i+bi;
			}
		}
	}
	return -1;
}

int main(){
	uint dword_size = 1024;
	unsigned int * s = malloc(dword_size*sizeof(int));
	bitset b; bitset_init(&b,s,dword_size);
	
	for(int i = 0; i < dword_size*32; i++){
			/*int v = bitset_search(&b); 
			if(i != v){
				fprintf(stderr,"%d != %d\n",i,v);
				return 0;
			}
			*/
			bitset_set(&b,i);
	}
	printf("%d %d\n",bitset_search(&b),_bitset_search(&b));
	if(bitset_search(&b) != _bitset_search(&b)){
		fprintf(stderr,"Wrong!\n");
		return 0;
	}

	free(s);
	printf("OK\n");
	return 0;
}
