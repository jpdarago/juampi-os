#include <list.h>
#include <memory.h>
#include <scrn.h>
#include <exception.h>
#include <utils.h>
#include <paging.h>

#define list_copy_body(type,bytes) \
    void * list_copy_ ## type(void * _data){ \
        type * new_data = kmalloc(sizeof(type)); \
        memcpy(new_data,_data,bytes); \
        return new_data; \
    }

/* Funciones auxiliares de duplicacion de tipos predefinidos */
list_copy_body(int,     sizeof(int))
list_copy_body(short,   sizeof(short))
list_copy_body(uint,    sizeof(uint))
list_copy_body(ushort,  sizeof(ushort))
list_copy_body(char,    sizeof(char))
list_copy_body(uchar,   sizeof(uchar))
list_copy_body(string,  strlen((char*)_data))
/* FIN */

list* _list_create(list_create_params p)
{
    list* f = kmalloc(sizeof(list));
    if(f == NULL) {
        return NULL;
    }
    f->head = f->tail = NULL;
    f->elements = 0;
    f->destroyer = p.destroyer;
    f->creator = p.creator;
    f->indexer = p.indexer;
    return f;
}

void list_destroy(list* f)
{
    list_node* backup;
    node_destroyer destroyer = f->destroyer;
    for(list_node* b = f->tail; b; b = backup) {
        backup = b->prev;
        if(destroyer) {
            destroyer(b->data);
        }
        kfree(b);
    }
    kfree(f);
}

uint list_add(list* bf, void* data)
{
    list_node* new_node = kmalloc(sizeof(list_node));
    if(new_node == NULL) {
        return -1;
    }
    void* copy = data;
    if(bf->creator) {
        copy = bf->creator(data);
    }
    new_node->data = copy;
    new_node->index = bf->elements;
    if(bf->indexer) {
        new_node->index = bf->indexer(data);
    }
    new_node->next = new_node->prev = NULL;
    if(bf->tail) {
        bf->tail->next = new_node;
        new_node->prev = bf->tail;
    }
    if(!bf->head) {
        bf->head = new_node;
    }
    bf->tail = new_node;
    bf->elements++;
    return new_node->index;
}

void* list_get(list* bf,uint index)
{
    list_foreach(l,bf) {
        if(l->index == index) {
            return l->data;
        }
    }
    return NULL;
}

static void _list_process_node(list* bf, uint index,
                               void (*action)(list*, list_node*))
{
    list_foreach(l,bf) {
        if(l->index == index) {
            action(bf,l);
            break;
        }
    }
}

static void unlink_node(list* bf, list_node* l)
{
    if(l == bf->head) {
        bf->head = bf->head->next;
    }
    if(l == bf->tail) {
        bf->tail = bf->tail->prev;
    }
    if(l->prev) {
        l->prev->next = l->next;
    }
    if(l->next) {
        l->next->prev = l->prev;
    }
}

static void destroy_node(list* bf, list_node* l)
{
    if(bf->destroyer) {
        bf->destroyer(l->data);
    }
    kfree(l);
    bf->elements--;
}

static void list_delete_node(list* bf, list_node* l)
{
    unlink_node(bf,l);
    destroy_node(bf,l);
}

void list_delete(list* bf, uint index)
{
    _list_process_node(bf,index,list_delete_node);
}

static void relink_to_back(list* bf, list_node* l)
{
    unlink_node(bf,l);
    l->next = l->prev = NULL;
    if(bf->tail) {
        bf->tail->next = l;
        l->prev = bf->tail;
    }
    bf->tail = l;
}

void list_move_back(list* bf, uint index)
{
    _list_process_node(bf,index,relink_to_back);
}

void* list_pop_front(list* bf)
{
    if(!bf->elements) {
        kernel_panic("Popeando una lista sin elementos");
    }
    void* data = bf->head->data;
    bf->head = bf->head->next;
    return data;
}
