#ifndef __RW_SEM_H
#define __RW_SEM_H

#include "types.h"
#include "sem.h"

//Tomado de The Little Book of Semaphores
//Basicamente implementa readers/writers sin starvation.
typedef struct {
	uint readers;
	sem * mutex;
	sem * room_empty;
	sem * turnstile;
} rw_sem;

rw_sem * rw_sem_create();
void rw_sem_destroy(rw_sem *);
void rw_sem_wlock(rw_sem *);
void rw_sem_wunlock(rw_sem *);
void rw_sem_rlock(rw_sem *);
void rw_sem_runlock(rw_sem *);

/*#define do_atomic_reader(sem,op)\
	do{	rw_sem_rlock(sem);\
  		op;\
		rw_sem_runlock(sem);\
	}while(0);

#define do_atomic_writer(sem,op)\
	do{	rw_sem_wlock(sem);\
  		op;\
		rw_sem_wunlock(sem);\
	}while(0);
*/

#define do_atomic_reader(sem,op) op
#define do_atomic_writer(sem,op) op

#endif
