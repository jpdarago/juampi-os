#ifndef __BUFFER_LIST_H
#define __BUFFER_LIST_H

#include <types.h>

typedef struct list_node {
    void * data;
    uint index;
    struct list_node * next, * prev;
} list_node;

typedef void (*node_destroyer)(void*);
typedef void*(*node_creator)(void*);
typedef uint (*node_indexer)(void*);

typedef struct {
    list_node * head, * tail;
    uint elements;
    node_destroyer destroyer;
    node_creator creator;
    node_indexer indexer;
} list;

typedef struct {
    node_destroyer destroyer;
    node_creator creator;
    node_indexer indexer;
} list_create_params;

//Crea la lista pasandole un creador (que lo que hace es una copia)
//y un destructor (que indica como destruir los nodos). Si creador
//es NULL, entonces la lista funciona por aliasing. Si el
//destructor es NULL, no se libera la memoria interna de los datos y
//eso queda a cargo del usuario.
list * _list_create(list_create_params);
#define list_create(...) \
    _list_create((list_create_params) { \
                     .destroyer = NULL, .creator = NULL, .indexer = NULL, \
                     __VA_ARGS__ })

//Destruye la lista
void list_destroy(list * f);
//Agrega un elemento al final de la lista y devuelve el indice
//identificador utilizado.
uint list_add(list * bf, void * data);
//Consigue el elemento que en campo indice tiene el valor
//pasado por parametro
void * list_get(list * bf,uint index);
//Quita de la lista el elemento que en el campo indice tiene
//el valor pasado por parametro
void list_delete(list * bf, uint index);
//Foreach para listas: Permite iterar un poco mas facilmente
#define list_foreach(i,l) for(list_node * i = l->head; i; i = i->next)
//Mueve el elemento de indice indicado al final de la lista
void list_move_back(list * bf, uint index);
//Saca el primer elemento de la lista
void * list_pop_front(list * bf);

typedef char * string;
#define list_copy_header(type) \
    void * list_copy_ ## type(void *)

list_copy_header(int);
list_copy_header(uint);

list_copy_header(char);
list_copy_header(uchar);

list_copy_header(short);
list_copy_header(ushort);

list_copy_header(string);

#endif
