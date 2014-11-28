#include <frames.h>
#include <bitset.h>
#include <exception.h>
#include <scrn.h>
#include <utils.h>

#define FRAME_SZ 0x1000
#define FRAME_MSK 0xFFF
#define FRAME_ALIGN(x) ((x) & ~0xFFF)

//Mapa de bits para los frames: 1 indica usado, 0 indica no usado
static bitset b;
//Direccion (fisica) donde empieza la memoria a administrar
static uint mem_start, total_count;
//Conteo de cantidad de personas a las cuales les asignamos un frame
static uint* count;

//Inicializa el mapa de bits de administracion y regresa la posicion
//del primer frame de datos utiles (o sea, calcula el tamaño del bitset)
uint frame_alloc_init(void* _mem_start, uint frames)
{
	uint mem = FRAME_ALIGN((uint) _mem_start + FRAME_SZ - 1);
	mem_start = mem;
	uint bitset_end = bitset_init(&b,(void*)mem,frames);
	count = (uint*) bitset_end;
	memset(count,0,frames*sizeof(uint));
	uint frame_alloc_finish = (uint)(count + frames);
	uint needed = CEIL(frame_alloc_finish - mem_start,FRAME_SZ);
	for(uint i = 0; i < needed; i++) {
		bitset_set(&b,i);
		count[i] = 1;
	}
	total_count = frames-needed;
	return mem_start+FRAME_SZ*needed;
}

uint frame_alloc()
{
	uint offset = bitset_search(&b);
	if(offset == (uint)-1) {
		kernel_panic("No hay frames disponibles para usar");
	}
	bitset_set(&b,offset);
	count[offset]++;
	total_count--;
	
	uint frame = mem_start+FRAME_SZ*offset;
	return frame;
}

void frame_free(uint frame)
{
	if(frame < mem_start) {
		kernel_panic("Se intento liberar un frame invalido");
	}
	uint offset = (frame-mem_start)/FRAME_SZ;
	if(--count[offset] == 0) {
		total_count++;
		bitset_clear(&b,offset);
	}
}

uint frames_available()
{
	return total_count;
}

void frame_add_alias(uint frame)
{
	if(frame < mem_start) {
		kernel_panic("Aliasing a frame invalido\n");
	}
	uint offset = (frame-mem_start)/FRAME_SZ;
	count[offset]++;
}
