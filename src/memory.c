#include <memory.h>
#include <frames.h>
#include <paging.h>
#include <scrn.h>
#include <exception.h>

// El manejador de memoria es una adaptacion minima del propuesto en el K & R.
// Inicializa el manejador de memoria del Kernel para funcionar sobre
// el espacio y tamanio dados.
kmem_map_header* kmem_init(void* memory_start, int memory_units)
{
    kmem_map_header* mem = memory_start;
    mem->freep = (kmem_header*)((char*) memory_start+sizeof(kmem_map_header));
    mem->freep->next = mem->freep;
    mem->freep->size = memory_units-sizeof(kmem_map_header)-sizeof(kmem_header);
    mem->heap_end = (intptr) memory_start + memory_units;
    return mem;
}

static kmem_header* best_fit_prev(kmem_map_header* mh,int units)
{
    kmem_header* ptr = mh->freep->next, * best = NULL;
    kmem_header* ptr_prev = mh->freep, * best_prev = NULL;
    kmem_header* stop = mh->freep;
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

// Pide mas memoria
static void* kmem_append_core(kmem_map_header* mh, uint units)
{
    uint pages = (units + 0xFFF) / 0x1000;
    if(!pages) {
        pages = 1;
    }
    kmem_header* mem = paging_append_core(mh,pages);
    mem->size = pages * 0x1000;
    mem->next = NULL;
    kmem_free(mh,(void*)mem);
    return mh->freep;
}

// Asigna memoria RAM libre de Kernel. Devuelve NULL si no hay memoria
void* kmem_alloc(kmem_map_header* mh, int size)
{
    if(size == 0) {
        return NULL;
    }
    // Necesito pedir el espacio para el encabezado tambien.
    int units = size + sizeof(kmem_header);
    // Encontrar bloque libre del tamanio necesario.
    // Implementacion: Best Fit
    kmem_header* best_prev = best_fit_prev(mh,units), * best;
    if(best_prev == NULL) {
        kmem_append_core(mh,units);
        best_prev = best_fit_prev(mh,units);
        if(best_prev == NULL) {
            kernel_panic("No hay mas heap de kernel");
            return NULL;
        }
    }
    best = best_prev->next;
    // Actualizar lista circular de bloques libres
    if(best->size == units) {
        if(best == best->next) {
            // El bloque es el unico de la lista libre.
            mh->freep = NULL;
        } else {
            mh->freep = best_prev;
            best_prev->next = best->next;
        }
    } else {
        best->size -= units;
        best = (kmem_header*)((intptr) best + best->size);
        best->size = units;
        mh->freep = best_prev;
    }
    return (void*)(best+1);
}

// Libera memoria RAM del Kernel. Es invalido tratar de devolver memoria
// no asignada con malloc.
void kmem_free(kmem_map_header* mh, void* p)
{
    if(p == NULL) {
        return;
    }
    kmem_header* bptr = (kmem_header*)p - 1;
    // Buscar con que bloque hay que enlazarlo.
    if(!mh->freep) {
        // No hay nadie libre. El unico bloque es el de p. A liberarlo
        mh->freep = bptr;
        bptr->next = bptr;
        return;
    }
    kmem_header* ptr = mh->freep;
    for(; bptr <= ptr || bptr >= ptr->next; ptr = ptr->next)
        if(ptr >= ptr->next && (bptr > ptr || bptr < ptr->next)) {
            break;
        }
    // Limpiar los bloques de los extremos que estan libres
    intptr bptr_addr = (intptr)bptr,ptr_addr = (intptr)ptr;
    if(bptr_addr + bptr->size == (intptr) ptr->next) {
        bptr->size += ptr->next->size;
        bptr->next = ptr->next->next;
    } else {
        bptr->next = ptr->next;
    }
    if(ptr_addr + ptr->size == (intptr) bptr) {
        ptr->size += bptr->size;
        ptr->next = bptr->next;
    } else {
        ptr->next = bptr;
    }
    // Marcar como nuevo tipo libre, para acelerar.
    mh->freep = ptr;
}

// Asigna memoria RAM libre de Kernel, alineada a pagina.
void* kmem_alloc_aligned(kmem_map_header* mh, int size)
{
    if(size == 0) {
        return NULL;
    }
    // Necesito pedir el espacio para el encabezado tambien.
    // Pido PAGE_SIZE - 1 adicionales asi me aseguro que en el
    // espacio de memoria obtenido va a haber un trozo alineado.
    uint units = size + PAGE_SZ - 1 + sizeof(kmem_header);
    // Encontrar bloque libre del tamanio necesario.
    // Implementacion: Best Fit
    kmem_header* best_prev = best_fit_prev(mh,units), * best;
    if(best_prev == NULL) {
        kmem_append_core(mh,units);
        best_prev = best_fit_prev(mh,units);
        if(best_prev == NULL) {
            kernel_panic("No hay mas memoria para asignar alineada");
            return NULL;
        }
    }
    best = best_prev->next;
    // Obtengo la direccion del pedazo de memoria
    intptr best_addr = (intptr) best, header_sz = sizeof(kmem_header);
    intptr block_addr= ALIGN(PAGE_SZ - 1 + header_sz + best_addr);
    intptr header_addr = block_addr - header_sz;
    intptr prev_size = best->size;
    kmem_header* header = (kmem_header*) header_addr;
    header->size = size+header_sz;
    if(best_addr != header_addr) {
        // Acomodar el trozo original para que tenga la memoria entre
        // el bloque original y el bloque alineado.
        best->size = header_addr - best_addr - header_sz;
    } else {
        // Como el original esta alineado, solo hay que acomodar para
        // el espacio que queremos y mover best_prev (como pedimos mas espacio
        // que el necesario seguro sobra).
        best_prev->next = (kmem_header*)(block_addr+size);
    }
    if(block_addr + size < best_addr + header_sz + prev_size) {
        // Sobra espacio seguido del bloque. Hay que crear un bloque nuevo que
        // contenga este espacio, y agregarlo a la lista de bloques libres.
        kmem_header* new_header = (kmem_header*)(block_addr + size);
        new_header->size = best_addr + prev_size -
                           (block_addr + size + header_sz);
        new_header->next = best->next;
        best->next = new_header;
    }
    mh->freep = best_prev;
    return (void*) block_addr;
}

uint kmem_available(kmem_map_header* mh)
{
    kmem_header* ptr = mh->freep;
    if(!ptr) {
        return 0;
    }
    uint total = 0;
    do {
        total += ptr->size;
        ptr = ptr->next;
    } while(ptr != mh->freep);
    return total;
}

void* kmalloc(uint size)
{
    kmem_map_header * kernel_heap = get_kernel_heap();
    void * res = kmem_alloc(kernel_heap,size);
    if(res == NULL)
        kernel_panic("No hay mas memoria para "
                     "alloc de %d bytes\n",size);
    return res;
}

void kfree(void* mem)
{
    if(mem == NULL) return;
    kmem_map_header * kernel_heap = get_kernel_heap();
    kmem_free(kernel_heap,mem);
}
