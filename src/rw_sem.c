#include <rw_sem.h>
#include <sem.h>
#include <paging.h>

rw_sem* rw_sem_create()
{
	rw_sem* res = kmalloc(sizeof(rw_sem));
	res->readers = 0;
	res->mutex      = sem_create(1);
	res->room_empty = sem_create(1);
	res->turnstile  = sem_create(1);
	return res;
}

void rw_sem_destroy(rw_sem* r)
{
	kfree(r->mutex);
	kfree(r->room_empty);
	kfree(r->turnstile);
	kfree(r);
}

void rw_sem_wlock(rw_sem* r)
{
	sem_wait(r->turnstile);
	sem_wait(r->room_empty);
}

void rw_sem_wunlock(rw_sem* r)
{
	sem_signal(r->turnstile);
	sem_signal(r->room_empty);
}

void rw_sem_rlock(rw_sem* r)
{
	sem_wait(r->turnstile);
	sem_signal(r->turnstile);
	sem_wait(r->mutex);
	r->readers++;
	if(r->readers == 1) {
		sem_wait(r->room_empty);
	}
	sem_signal(r->mutex);
}

void rw_sem_runlock(rw_sem* r)
{
	sem_wait(r->mutex);
	r->readers--;
	if(r->readers == 0) {
		sem_signal(r->room_empty);
	}
	sem_signal(r->mutex);
}
