#include "sem.h"
#include <stdlib.h>

//TODO: Agregar irq_cli y irq_sti para asegurar atomicidad
sem * sem_create(int initial_value)
{
	sem * s = malloc(sizeof(sem));

	s->value = initial_value;
	INIT_LIST_HEAD(&s->waiting);
	return s;
}

void sem_wait(sem * s)
{
	//TODO: Agregar espera de proceso
}

void sem_destroy(sem * s)
{
	free(s);
}

void sem_signal(sem * s)
{
	//TODO: Agregar liberacion de semaforo
}
