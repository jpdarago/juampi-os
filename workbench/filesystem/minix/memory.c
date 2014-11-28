#include "memory.h"
#include <stdio.h>

//Puntero a la lista libre de bloques
//El manejador de memoria es una adaptacion minima del propuesto en el K & R.
typedef struct kmem_Header {
	struct kmem_Header * next;
	uint size;
} kmem_Header;

static kmem_Header * freep = NULL;
static const uint addressable_size = 1;

//Inicializa el manejador de memoria del Kernel para funcionar sobre
//el espacio y tamanio dados.
void kmem_init(void * memory_start, uint memory_units)
{
	freep = (kmem_Header *) memory_start;
	freep->next = freep;
	freep->size = memory_units * addressable_size;
}

static kmem_Header * best_fit_prev(uint units)
{
	kmem_Header * ptr = freep->next, * best = NULL;
	kmem_Header * ptr_prev = freep, * best_prev = NULL;
	kmem_Header * stop = freep;
	do {
		if(ptr->size >= units && (!best || ptr->size < best->size)) {
			best = ptr;
			best_prev = ptr_prev;
		}
		ptr = ptr->next;
		ptr_prev = ptr_prev->next;
	} while(ptr_prev != stop);
	return best_prev;
}

//Asigna memoria RAM libre de Kernel. Devuelve NULL si no hay memoria
void * kmem_alloc(uint size)
{
	if(size == 0) return NULL;
	//Necesito pedir el espacio para el encabezado tambien.
	uint units = size + sizeof(kmem_Header);

	//Encontrar bloque libre del tamanio necesario.
	//Implementacion: Best Fit
	kmem_Header * best_prev = best_fit_prev(units);
	if(best_prev == NULL) {
		return NULL;
	}
	kmem_Header * best = best_prev->next;
	//Actualizar lista circular de bloques libres
	if(best->size == units) {
		if(best == best->next) {
			//El bloque es el unico de la lista libre.
			freep = NULL;
		} else {
			freep = best_prev;
			best_prev->next = best->next;
		}
	} else {
		best->size -= units;
		best = (kmem_Header *) ((char*)best + best->size);
		best->size = units;
		freep = best_prev;
	}
	return (void*) (best+1);
}

//Libera memoria RAM del Kernel. Es invalido tratar de devolver memoria
//no asignada con malloc.
void kmem_free(void * p)
{
	if(p == NULL) return;
	kmem_Header * bptr = (kmem_Header*)p - 1;
	//DEBUG("LIBERANDO BLOQUE QUE EMPIEZA EN LA DIRECCION %p DE TAMANIO %d\n",bptr+1,bptr->size);
	//Buscar con que bloque hay que enlazarlo.
	if(!freep) {
		//No hay nadie libre. El unico bloque es el de p. A liberarlo
		freep = bptr;
		bptr->next = bptr;
		return;
	}
	kmem_Header * ptr = freep;
	do {
		if(bptr >= ptr && bptr <= ptr->next)
			break;
	} while(ptr != freep);

	//Limpiar los bloques de los extremos que estan libres
	uint bptr_addr = (uint)bptr,ptr_addr = (uint)ptr;
	if(bptr_addr + bptr->size == (uint) ptr->next) {
		bptr->size += ptr->next->size;
		bptr->next = ptr->next->next;
	} else {
		bptr->next = ptr->next;
	}
	if(ptr_addr + ptr->size == (uint) bptr) {
		ptr->size += bptr->size;
		ptr->next = bptr->next;
	} else {
		ptr->next = bptr;
	}
	//Marcar como nuevo tipo libre, para acelerar.
	freep = ptr;
}

#define MASK_PALIGN ~0xFFF
//Tamanio en bytes de una pagina.
#define PAGE_SIZE 0x1000
//Asigna memoria RAM libre de Kernel, alineada a pagina.
void * kmem_alloc_aligned(uint size)
{
	if(size == 0) return NULL;
	//Necesito pedir el espacio para el encabezado tambien.
	//Pido PAGE_SIZE - 1 adicionales asi me aseguro que en el
	//espacio de memoria obtenido va a haber un trozo alineado.
	uint units = size + PAGE_SIZE - 1 + sizeof(kmem_Header);
	//Encontrar bloque libre del tamanio necesario.
	//Implementacion: Best Fit
	kmem_Header * best_prev = best_fit_prev(units);
	if(best_prev == NULL) {
		return NULL;
	}
	kmem_Header * best = best_prev->next;
	//Obtengo la direccion del pedazo de memoria
	uint best_addr = (uint) best, header_sz = sizeof(kmem_Header);
	uint block_addr= (PAGE_SIZE - 1 + header_sz + best_addr) & MASK_PALIGN;
	uint header_addr = block_addr - header_sz;

	uint prev_size = best->size;

	kmem_Header * header = (kmem_Header * ) header_addr;
	header->size = size+header_sz;

	if(best_addr != header_addr) {
		//Acomodar el trozo original para que tenga la memoria entre
		//el bloque original y el bloque alineado.
		best->size = header_addr - best_addr - header_sz;
	} else {
		//Como el original esta alineado, solo hay que acomodar para
		//el espacio que queremos y mover best_prev (como pedimos mas espacio
		//que el necesario seguro sobra).
		best_prev->next = (kmem_Header *) (block_addr+size);
	}
	if(block_addr + size < best_addr + header_sz + prev_size) {
		//Sobra espacio seguido del bloque. Hay que crear un bloque nuevo que
		//contenga este espacio, y agregarlo a la lista de bloques libres.
		kmem_Header * new_header = (kmem_Header *) (block_addr + size);
		new_header->size = best_addr + prev_size -
		                   (block_addr + size + header_sz);
		new_header->next = best->next;
		best->next = new_header;
	}
	freep = best_prev;
	return (void *) block_addr;
}

//Obtiene la cantidad de memoria disponible
uint kmem_available()
{
	kmem_Header * ptr = freep;
	uint total = 0;
	do {
		total += ptr->size;
		ptr = ptr->next;
	} while(ptr != freep);
	return total;
}
