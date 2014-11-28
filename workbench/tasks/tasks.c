#include "tasks.h"
#include "memory.h"
#include "paging.h"
#include "tss.h"
#include "irq.h"
#include "gdt.h"
#include "exception.h"
#include "elf.h"
#include "list.h"

#define KSTACK_START 	0xF0000000
#define KSTACK_PAGES	8
#define KSTACK_SIZE		(KSTACK_PAGES*PAGE_SZ)

extern kmem_map_header * kernel_heap; 
static int last_pid = 0;

void scheduler_init(){
	processes = list_create();
}

//Consigue el selector de segmento de la tarea actual:
//Al venir de un fork, este selector identifica a la tarea
//padre del proceso hijo que vamos a crear.
static ushort get_tr(){
	ushort res;
	__asm__ __volatile__ ("str %0" : "=r"(res));
	return res;
}

//Devuelve el PID de la tarea que tiene selector de 
//segmento dado por tss_selector
static process_info * search_tss(uint tss_selector){
	list_foreach(i,processes){
		process_info * p = i->data;
		if(p->tss_selector == tss_selector)
			   return p;
	}
	return NULL;
}

//El manual de intel en la pagina 304 dice
//	Avoid placing a page boundary in the part of the
//	TSS that the processor reads during a task switch
//	(the first 104 bytes).
//Entonces para obtener una tss valida debemos pedir no
//104 sino 209 bytes. Seguro que si pedimos esa cantidad va
//a haber un pedazo contiguo de 104 (por ser las paginas de
//4 Kb). Pero hay que determinar que pedazo es contiguo.
//Para eso sirven las siguientes dos funciones.
//Son dos separadas porque no queremos perder memoria: cuando
//matemos un proceso vamos a liberar su tss tambien.
static void * new_tss_space(){
	return kmem_malloc(2*sizeof(tss)+1);
}

static tss * get_contiguous_tss(void * space){
	uint pas  = physical_address(current_dir,(uint)space);
	uint panb = physical_address(
					current_dir,(uint)space+sizeof(tss));
	uint fas = ALIGN(pas), fanb = ALIGN(panb);
	if(fas == fanb || fas == fanb+1) return space;
	return (void*)((uint)space+sizeof(tss));
}

//Crea una nueva pila de kernel que tiene su tope
//en la direccion dada por KSTACK_START (la pila crece al
//reves en intel).
static int
create_kernel_stack(page_directory * new_dir, tss * new_tss)
{
	//Conseguimos una nueva pila de kernel para este proceso,
	//de 8 paginas, para cuando necesitemos hacer cambios de
	//contexto por interrupcciones.
	
	void * ksspace  = kmem_alloc_aligned(kernel_heap,KSTACK_SIZE);
	if(ksspace == NULL) return -1;

	for(uint i = 0; i < KSTACK_PAGES; i++){
		uint sp = KSTACK_START + i*PAGE_SZ, 
			 khp= (uint)ksspace + i*PAGE_SZ;

		map_page(new_dir,i,
			physical_address(current_dir,kspace+i),
			PAGEF_P | PAGEF_RW);	
	}
	
	new_tss->esp0 = ksspace + 8*PAGE_SZ;
	new_tss->ss0  = DATA_SEGMENT_KERNEL;

	return 0;
}

//Inicializa una nueva TSS con los valores para un nuevo proceso
//duplicado, con los valores obtenidos de la traza.
static int 
init_tss(gen_regs * regs, int_trace * trace, tss ** _new_tss)
{
	new_tss * = *_new_tss;
	memset(new_tss,0,sizeof(tss));
	
	new_tss->cs = trace->cs;
	new_tss->eflags = trace->eflags;	
	new_tss->ss = new_tss->es = new_tss->ds =
	new_tss->fs = new_tss->fs = trace->ss;
		
	new_task->ebx = regs->ebx;
	new_task->ecx = regs->ecx;
	new_task->edx = regs->edx;
	new_task->esi = regs->esi;
	new_task->edi = regs->edi;
	new_task->ebp = regs->ebp;
	
	new_task->esp = trace->usersp;
	new_task->eip = trace->eip;
	new_tss->cr3 = new_dir->physical_address; 
	
	int cks = create_kernel_stack(new_dir,new_tss);
	if(cks) return cks;
	
	//No ponemos mapa de io para que las tareas 
	//no puedan utilizar los puertos de entrada salida
	new_tss->iomap_addr = sizeof(tss);
	
	return gdt_add_tss(physical_address(current_dir,tss));
}

process_info * 
create_process_info(ushort tss_index, void * tss_space)
{
	process_info * pi = 
		kmem_alloc(kernel_heap,sizeof(process_info));
	process_info * parent = search_tss(ltr());

	pi->status = P_AVAILABLE;
	pi->pid = last_pid++;
	pi->childs_pid = list_create();
	pi->parent_pid = parent ? parent->pid : -1; 
	pi->tss_index = tss_index;
	pi->tss_space_start = tss_space;
	pi->remaining_quantum = START_QUANTUM;

	return pi;
}

int do_fork(gen_regs regs, int_trace trace)
{
	//Clonar el directorio de paginas usando el mecanismo
	//de copy on write que utiliza el page fault handler.
	page_directory * new_dir = clone_directory(current_dir);

	//Crear la TSS para el nuevo proceso
	void * space = new_tss_space();	
	tss * new_tss = get_contiguous_tss(space);
	if(new_tss == NULL) return -1;

	int tss_index = init_tss(&regs,&trace,&new_tss);

	//Agregar el proceso a la lista de procesos
	process_info * pi = new_process_info(tss_index,space);
	list_add(processes,pi);
	
	//El hijo recibe pid 0 al forkear (asi sabemos que es el hijo)
	new_task->eax = 0;
	//El padre recibe el pid original
	return pi->pid;
}

//Determina que flags tiene que tener la pagina dado el tipo de
//segmento que estamos mirando
uint determine_flags(elf_segment * es){
	uint eflags = PAGE_P; //Presente esta seguro
	if(~es->flags & ELF_ATTR_RB) return flags;
	flags |= PAGEF_U; //Si se puede leer seguro es de usuario
	if(es->flags & ELF_ATTR_WB) flags |= PAGE_RW;
	return flags;
}

//Pega la imagen dada por el archivo elf (en memoria, indicado
//por el buffer pasado por parametro) al mapa de memoria del 
//proceso actual.
void elf_overlay_image(void * elf_image_buffer)
{
	elf * elf = elf_exec_create(elf_image_buffer);
	if(elf == NULL) return -1;
	for(uint i = 0; i < elf->ph_entry_count; i++){
		elf_segment * e = elf_get_segment(elf,i);

		if(e->type == ELF_LOAD){
				uint address,start_address;
				address = start_address = 
						e->virtual_address;
				
				for(;address < start_address + e->mem_size; 
						address=NEXT_ALIGN(address)){
				
					uint new_page = kmem_alloc_aligned(PAGE_SZ);
					uint new_frame= 
						physical_address(current_dir,address);
					
					uint flags = determine_flags(e);
					map_page(current_dir, address, new_frame, flags);
				}
		}
		
		elf_free_segment(e);
	}
}	

//Consigue el buffer de una imagen dada su nombre. Esto es
//para propositos de testing internos: queremos evitar el
//uso de disco duro para estas cosas.
void * fetch_internal(char * filename)
{
	return NULL;
}

//Crea una una imagen de proceso usando el mapa de directorio
//actual. Efectivamente carga el ejecutable pasado por parametro.
void exec(char * filename)
{
	if(strcmp(filename,"task")){
		kernel_panic("Todavia no esta integrada "
					  "la funcionalidad de filesystem");
	}
	process_info * p = search_tss(ltr());
	if(p == NULL){
		kernel_panic("Haciendo exec con un proceso invalido");
	}
	void * buffer = fetch_internal(filename);
	elf_overlay_image(buffer);
	p->eip = elf_entry_point(elf);
	kmem_free(kernel_heap,buffer);
}

void wait(uint child_pid)
{
	return;
}

static inline int still_running(parent_info * p)
{
	return p && p->current_task == P_RUNNING && p->quantum > 0;
}

static inline int add_to_round_robin(parent_info * p)
{
	return p && p->status == P_RUNNING;
}

short next_task()
{
	parent_info * current_task = search_tss(ltr());
	
	if(still_running(current_task)){
		current_task->quantum--;
		return current_task->tss_selector;
	}else{
		if(add_to_round_robin(current_task)){	
			current_task->quantum = START_QUANTUM;
			current_task->status = P_AVAILABLE;
		}
		list_foreach(l,processes){
			if(l == current_task) continue;
			if(l->P_AVAILABLE && l->quantum > 0){
				l->status = P_RUNNING;
				return l->tss_selector;
			}
		}
	}
	return -1;
}
