#include "bitset.h"
#include "frames.h"
#include "exception.h"

#define FRAME_SZ 0x1000
#define FRAME_MSK 0xFFF

static bitset b;
static uint mem_start;

//Inicializa el administrador de memoria
void frame_alloc_init(void * _mem_start, uint frames){
	uint mem = (uint) _mem_start;	
	mem = (mem+0xFFF) & ~0xFFF; 
	
	mem_start = (uint) mem + FRAME_SZ;
	
	//Dejamos una pagina para el bitset
	//Una pagina tiene 4096 bytes = 32768 bits, con
	//lo cual podemos atender 32768 paginas de 4K lo
	//cual en total nos da una administracion de hasta 128
	//megabytes lo cual quiere decir que nos alcanza 
	//ampliamente a nuestros propositos
	if(frames > FRAME_SZ*8)
		kernel_panic("Bitset insuficiente");
	
	//El tamaño del bitset se da en uint de 32 bits = 4 bytes
	bitset_init(&b,mem,FRAME_SZ/4);
}

//Devuelve y asigna un frame
uint frame_alloc(){
	uint offset = bitset_search(&b);
	if(offset == (uint)-1){ 
		kernel_panic("No hay frames disponibles");
		return offset;
	}
	bitset_set(&b,offset);	
	return mem_start+offset;
}

//Libera un frame asignado previamente
uint frame_free(uint frame){
	uint frame = ptr-mem_start;
	bitset_clear(&b,offset);
}
