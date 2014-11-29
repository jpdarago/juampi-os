#include <bitset.h>
#include <utils.h>
#include <scrn.h>

#define DWORD_SZ 32

//Esta funcion inicializa solamente los parametros de
//offsets de un bitset. Es porque se utiliza para minix
//donde no queremos borrar el pedazo final del bitmap
//porque minix ya nos lo da bien
uint bitset_load(bitset* b, void* start, uint size)
{
    b->start = start;
    b->size = CEIL(size,DWORD_SZ);
    return (uint)(b->start + b->size);
}

//Inicializa el bitset para que utilice el bitmap
//cargado en el cacho de memoria que empieza en start.
//El size es la cantidad de cosas a administrar
uint bitset_init(bitset* b, void* start, uint size)
{
    uint res = bitset_load(b,start,size);
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
