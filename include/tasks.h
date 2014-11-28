#ifndef __TASK_H
#define __TASK_H

#include <types.h>
#include <klist.h>
#include <tss.h>
#include <elf.h>
#include <paging.h>
#include <vfs.h>
#include <proc.h>
#include <fs.h>

typedef enum status {
	P_RUNNING,
	P_BLOCKED,
	P_AVAILABLE,
	P_COMA
} status;

typedef void (*signal_handler)(int);

#define SIGNALS 4

#define SIGINT 	0
#define SIGKILL 1
#define SIGSTOP	2
#define SIGCONT	3

#define ERRINVPID 		-1
#define ERRINVSIG 		-2
#define ERRIGNSIG 		-3
#define ERROUTMEM 		-4
#define ERRGDTFULL		-5
#define ERRINVFILE		-6
#define ERRBIGEXEC		-7
#define ERRNOTELF		-8
#define ERRREAD			-9
#define ERRIMPOSSIBLE	-10
#define ERRARGTOOBIG	-11
#define ERRTOOMANYARGS	-12
#define ERRREADINGELF	-13

#define EXEC_MAX_FSIZE (1024*1024)
#define EXEC_MAX_ARGC	16
#define EXEC_MAX_ARGSZ	128

typedef struct process_info {
	//Identificador de proceso
	int pid;
	//Estado del proceso:
	//	P_RUNNING: Tiene actualmente la CPU
	//	P_BLOCKED: Bloqueado por I/O
	//	P_AVAILABLE: Disponible y listo para correr
	status status;
	//Lista de los pid de los procesos hijos
	list_head children;
	//Estructura con la informacion del proceso padre	
	struct process_info * parent;
	//Selector de TSS correspondiente a este proceso
	short tss_selector;
	//Espacio de memoria donde esta la tss de este
	//proceso y la tss explicita del proceso
	void * tss_space_start;
   	tss	* tss;
	//Cuanto tiempo restante tiene este proceso. Solo tiene
	//sentido cuando status = P_RUNNING
	uint remaining_quantum;
	//Puntero al directorio de paginas
	page_directory * page_dir;	
	//Puntero al hijo que esta esperando si esta 
	//esperando a un hijo
	struct process_info * waiting_child;
	//Handlers de señales
	signal_handler signal_handlers[SIGNALS];
	//Bitmap de señales pendientes a atender y a ignorar (porque
	//ya se esta procesando una de ese tipo)
	int signal_bitmap, ignore_bitmap;
	//Flag que dice si el proceso esta corriendo 
	//en modo kernel, para que no lo preempteen si es asi 
	bool kernel_mode;
	//Valores donde estan el esp y eip cuando vuelva de 
	//la system call (solo valido si esta en kernel mode)
	uint * prev_esp_pos, * prev_eip_pos;
	//Posicion en la lista de procesos
	list_head process_list;	
	//Posicion en la lista de procesos del padre
	list_head parent_list;
	//Posicion en la lista de semaforos donde duerme
	list_head sem_queue;
	//Lista de file objects. Cada numero es un file
	//descriptor abierto
	file_object fds[MAX_FDS];
	//Directorio actual (current working directory)
	char cwd[FS_MAXLEN];
} __attribute__((__packed__)) process_info;

#define START_QUANTUM 50

extern list_head processes;

//Inicializa el scheduler de procesos
void scheduler_init();
//Helper para forkear
int do_fork(uint,gen_regs,uint,uint);
//Cambiar la imagen de proceso actual por la pasada 
//por parametro, con los argumentos en la pila indicados. 
//Devuelve 0 si tuvo exito, -1 si fallo
int do_exec(char * filename, char ** args, int_trace *,uint *);
//Esperar por el proceso hijo de pid indicado
int do_wait(uint child_pid);
//Devuelve la tss de la proxima tarea a ejecutar.
process_info * next_task();
//Pega la imagen del buffer de archivo elf pasado por parametro
int elf_overlay_image(elf_file * e);
//Devuelve un puntero a la estructura de la tarea actual, NULL si
//no hay tarea inicial
process_info * get_current_task();
//Salta de tarea
void perform_task_switch(process_info *);
//Salta a la tarea inicial
void jump_to_initial(void *);
//Mata al proceso actual
int do_exit();

//Duerme al proceso actual (liberando asi su quantum).
int do_sleep();

//Bloquea al proceso actual y libera su quantum
void block_current_process();
//Desbloquea al proceso actual
void wake_up(process_info * p);

void switch_kernel_mode();
void switch_user_mode();
bool kernel_mode();
bool is_preemptable();

//Signal handling
int do_kill(int pid, int signal);
int do_signal(int signal, signal_handler); 

//Estas dos funciones son kludges para implementar una syscall que
//deseschedulee a un proceso (usado para manejar las señales SIGSTOP
//y SIGCONT).
int do_coma();
int do_clear_signal(int signal);

//Directorio actual de trabajo del proceso actual
void do_get_cwd(char * buf);
int do_set_cwd(const char *);

//Mi pid
int do_get_pid();

#endif
