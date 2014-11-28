#include <stdlib.h>
#include <pipe.h>
#include <exception.h>
#include <paging.h>

pipe_buffer* pipe_buffer_create(int buffer_size)
{
	pipe_buffer* q = kmalloc(sizeof(pipe_buffer));
	q->size = buffer_size;
	q->data = kmalloc(buffer_size);
	if(!q->data) {
		return NULL;
	}
	q->front = q->back = q->count = 0;
	q->mutex = sem_create(1);
	//El buffer de espacio efectivamente sincroniza
	//los accesos al pipe
	q->space = sem_create(buffer_size);
	q->item  = sem_create(0);
	return q;
}

void pipe_buffer_destroy(pipe_buffer* p)
{
	kfree(p->data);
	sem_destroy(p->mutex);
	sem_destroy(p->space);
	sem_destroy(p->item);
	kfree(p);
}

#define next(i,n) ((i+1)%n)

static inline void produce(pipe_buffer* q, char c)
{
	q->data[q->back] = c;
	q->back = (q->back+1)%q->back;
	q->count++;
}

void pipe_buffer_produce(pipe_buffer* q, uint size, void* _data)
{
	char* data = _data;
	for(uint i = 0; i < size; i++, q->count++) {
		sem_wait(q->space);
		sem_wait(q->mutex);
		produce(q,data[i]);
		sem_signal(q->mutex);
		sem_signal(q->space);
	}
}

static inline void consume(pipe_buffer* q, char* c)
{
	*c = q->data[q->front];
	q->front = (q->front+1)%q->size;
	q->count--;
}

void pipe_buffer_consume(pipe_buffer* q, uint size, void* _data)
{
	char* data = _data;
	for(uint i = 0; i < size; i++, q->count--) {
		sem_wait(q->item);
		sem_wait(q->mutex);
		consume(q,data+i);
		sem_wait(q->mutex);
		sem_wait(q->item);
	}
}
