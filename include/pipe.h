#ifndef __PIPE_H
#define __PIPE_H

#include <types.h>
#include <sem.h>

typedef struct {
    char *data;
    int front,back,size,count;
    sem * mutex;
    sem * space;
    sem * item;
} pipe_buffer;

pipe_buffer * pipe_buffer_create(int);
void pipe_buffer_destroy(pipe_buffer *);
void pipe_buffer_produce(pipe_buffer *,uint,void*);
void pipe_buffer_consume(pipe_buffer *,uint,void*);

#endif
