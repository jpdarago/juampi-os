#ifndef __BITSET_H
#define __BITSET_H

#include <types.h>

typedef struct {
    uint32 * start;
    uint32 size;
} bitset;

//Inicializa el bitset: El tamaño es la
//cantidad de cosas a administrar. Devuelve la
//direccion de memoria donde termina el bitset
uint32 * bitset_init(bitset * b, void * start, uint size);
//Carga un bitset dada memoria ya existente (para MINIX por ejemplo)
uint32 * bitset_load(bitset *, void *, uint);
//Marca el bit como usado
void bitset_set(bitset * b, uint index);
//Marca el bit como libre
void bitset_clear(bitset * b, uint index);
//Devuelve el indice de un bit libre en
//el mapa de bits. Debido a su importancia
//esta en bitset_search en assembler.
uint bitset_search(bitset * b);

#endif
