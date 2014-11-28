#include "list.h"
#include "memory.h"
#include <assert.h>

list * _list_create(list_create_params p){
	list * f = kmem_alloc(sizeof(list));
	if(f == NULL) return NULL;
	f->head = f->tail = NULL;
	f->elements = 0;

	f->destroyer = p.destroyer;
	f->creator = p.creator;

	return f;
}

void list_destroy(list * f){
	list_node * backup;
	node_destroyer destroyer = f->destroyer;
	for(list_node * b = f->tail; b; b = backup){
		backup = b->prev;
		if(destroyer){ 
				destroyer(b->data);
		}
		kmem_free(b);
	}
	kmem_free(f);
}

uint list_add(list * bf, void * data){
	list_node * new_node = kmem_alloc(sizeof(list_node));
	if(new_node == NULL) return -1;
	void * copy = data;
	if(bf->creator) 
			copy = bf->creator(data);
	new_node->data = copy;
	new_node->index = bf->elements;
	new_node->next = new_node->prev = NULL;

	if(bf->tail){	
		bf->tail->next = new_node;
		new_node->prev = bf->tail;
	}
	
	if(!bf->head) bf->head = new_node;
	bf->tail = new_node;
	bf->elements++;
	return new_node->index;
}

void * list_get(list * bf,uint index){
	for(list_node * l = bf->head; l; l = l->next){
		if(l->index == index){ 
			return l->data;
		}
	}
	return NULL;
}

void list_delete(list * bf, uint index){
	for(list_node * l = bf->head; l; l = l->next){
		if(l->index == index){
			if(l == bf->head) bf->head = bf->head->next;
			if(l == bf->tail) bf->tail = bf->tail->prev;
			if(l->prev){
				l->prev->next = l->next;
			}
			if(l->next){
				l->next->prev = l->prev;
			}

			if(bf->destroyer) 
					bf->destroyer(l->data);

			kmem_free(l);
			bf->elements--;
			break;
		}
	}
}
