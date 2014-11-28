#ifndef __TASK_H
#define __TASK_H

#include "types.h"

typedef enum status {P_RUNNING,P_BLOCKED,P_AVAILABLE} status;
typedef struct {
	int pid;
	status status;
	list * childs_pid;
	uint parent_pid;
	ushort tss_selector;
	//Ver funcion create_new_tss
	tss * tss_space_start;
	uint quantum;
} __attribute__((__packed__)) process_info;

#define START_QUANTUM 100

extern list * processes; 

//Inicializa el scheduler de procesos
void scheduler_init();
//Forkear el proceso actual. El padre recibe el nuevo pid del
//hijo, el hijo recibe pid 0. Si hay un error el padre recibe
//-1 y el hijo no se crea.
int fork();
//Cambiar la imagen de proceso actual por la pasada 
//por parametro. Devuelve 0 si tuvo exito, -1 si fallo
int exec(char * filename);
//Esperar por el proceso hijo de pid indicado
void wait(uint child_pid);
//Devuelve la tss de la proxima tarea a ejecutar.
short next_task();
//Pega la imagen del buffer de archivo elf pasado por parametro
void elf_overlay_image(void * elf_mem_buffer);

#endif
