#ifndef __MEM_H
#define __MEM_H

#include <types.h>
#include <klist.h>
#include <paging.h>

typedef struct {
	int value;
	list_head wait_queue;
} __attribute__((__packed__)) sem;

sem * sem_create(int initial_value);
void sem_wait(sem * s);
void sem_signal(sem * s);
void sem_destroy(sem * s);
/*
#define do_atomic(sem,op)\
	do{ sem_wait(sem);\
  		op;\
		sem_signal(sem);\
	}while(0);
*/
#define do_atomic(sem,op) op
#endif
