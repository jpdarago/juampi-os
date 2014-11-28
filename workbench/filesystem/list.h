#ifndef __BUFFER_LIST_H
#define __BUFFER_LIST_H

#include "types.h"
#include <stdlib.h>
#include <stdio.h>

typedef struct list_node {
	void * data;
	uint index;
	struct list_node * next, * prev;
} list_node;

typedef void (*node_destroyer)(void*);
typedef void*(*node_creator)(void*);

typedef struct {
	list_node * head, * tail;
	uint elements;
	node_destroyer destroyer;
	node_creator creator;
} list;

typedef struct {
	node_destroyer destroyer;
	node_creator creator;
} list_create_params;

list * _list_create(list_create_params);
#define list_create(...)\
	_list_create((list_create_params) {\
		.destroyer = NULL, .creator = NULL, __VA_ARGS__ })

void list_destroy(list * f);
uint list_add(list * bf, void * data);
void * list_get(list * bf,uint index);
void list_delete(list * bf, uint index);

#define list_foreach(i,l) for(list_node * i = l->head; i; i = i->next)

#endif
