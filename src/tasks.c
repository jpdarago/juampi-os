#include <tasks.h>
#include <memory.h>
#include <paging.h>
#include <tss.h>
#include <irq.h>
#include <gdt.h>
#include <exception.h>
#include <elf.h>
#include <utils.h>
#include <frames.h>
#include <scrn.h>
#include <proc.h>
#include <bochs_debug.h>
#include <fdesc.h>
#include <fs.h>

#define USTACK_START    0xE0000000
#define USTACK_PAGES    16
#define USTACK_SIZE     (USTACK_PAGES*PAGE_SZ)
#define USTACK_BASE     (USTACK_START+USTACK_SIZE)
#define KSTACK_START    (USTACK_START+USTACK_SIZE+PAGE_SZ)
#define KSTACK_PAGES    8
#define KSTACK_SIZE     (KSTACK_PAGES*PAGE_SZ)
#define KSTACK_BASE     (KSTACK_START+KSTACK_SIZE)

//Tss de la tarea inicial (el shell) y la de
//kernel (que no hace nada y es un sentinela).
tss initial_task, kernel_tss;

static process_info* current_process = NULL;
list_head processes;

static int last_pid = 1;

bool preemptable;

//Pasa el procesador a modo kernel
void switch_kernel_mode()
{
    uint eflags = irq_cli();
    if(current_process) {
        preemptable = !current_process->kernel_mode;
        current_process->kernel_mode = true;
    } else {
        preemptable = true;
    }
    irq_sti(eflags);
}

static void handle_signals_kernel_mode(process_info* p, intptr*, intptr*);

//Pasa el procesador a modo usuario
void switch_user_mode(int_trace* t)
{
    uint eflags = irq_cli();
    if(current_process) {
        preemptable = true;
        if(current_process->signal_bitmap) {
            handle_signals_kernel_mode(current_process,
                                       &t->useresp,&t->eip);
        }
        current_process->kernel_mode = false;
    }
    irq_sti(eflags);
}

//Determina si el procesador esta en modo kernel
bool kernel_mode()
{
    if(!current_process) {
        return true;
    }
    return current_process->kernel_mode;
}

bool is_preemptable()
{
    return preemptable;
}

//Agrega el proceso a la lista de procesos a
//considerar para schedulear
static void add_process(process_info* p)
{
    list_add(&p->process_list,&processes);
}

//Consigue el proceso dado su pid
static process_info* get_process_by_pid(int pid)
{
    process_info* cand;
    list_for_each_entry(cand,&processes,process_list) {
        if(cand->pid == pid) {
            return cand;
        }
    }
    return NULL;
}

//Pone al proceso como scheduleable, esto se hace para
//volver por ejemplo de bloqueos y sleeps
static void set_runnable(process_info* p)
{
    p->remaining_quantum = START_QUANTUM;
    p->status = P_AVAILABLE;
}

static void process_add_child(process_info* parent,
                              process_info* child)
{
    list_add(&child->parent_list,&parent->children);
}

static void process_remove_child(process_info* parent,
                                 process_info* child)
{
    list_del(&child->parent_list);
}

static void remove_from_process_tree(process_info* p)
{
    process_info* parent = p->parent, * ptr;
    if(parent) {
        if(parent->waiting_child == p) {
            parent->waiting_child = NULL;
            set_runnable(parent);
        }
        process_remove_child(parent,p);
        list_for_each_entry(ptr,&p->children,
                            parent_list)
        process_add_child(ptr,parent);
    }
    list_del(&p->process_list);
}
static bool in_stack_page(uint cpage)
{
    if(cpage >= KSTACK_START && cpage < KSTACK_BASE) return true;
    if(cpage >= USTACK_START && cpage < USTACK_BASE) return true;
    return false;
}

static void free_process_data_frames(process_info* proc)
{
    page_directory* p = proc->page_dir;
    for(uint pdi = 0; pdi < PAGE_TABLES; pdi++) {
        page_table* pt = p->tables_virtual[pdi];
        if(!pt || pt == kernel_dir->tables_virtual[pdi])
            continue;

        bool should_free = true;
        for(uint pti = 0; pti < PAGE_FRAMES; pti++) {
            page_entry* pe = &pt->entries[pti];
            if(!pe->present) continue;
            uint cpage = (pdi << 22) + (pti << 12);

            //No actuamos sobre la pila de kernel porque si
            //lo hicieramos podriamos potencialmente quebrar el
            //stack frame para las syscalls que usen esta funcion
            //Tampoco por la pila de usuario
            if(in_stack_page(cpage)) {
                should_free = false;
                continue;
            }

            clear_page_entry(p,pdi,pti);
        }
        if(should_free)
            clear_table_entry(p,pdi);
    }
}

static void free_process(process_info* p)
{
    remove_from_process_tree(p);
    page_directory_destroy(p->page_dir);
    if(p->tss_space_start) {
        kfree(p->tss_space_start);
    }
    kfree(p);
}

//El manual de intel en la pagina 304 dice
//  Avoid placing a page boundary in the part of the
//  TSS that the processor reads during a task switch
//  (the first 104 bytes).
//Entonces para obtener una tss valida debemos pedir no
//104 sino 209 bytes. Seguro que si pedimos esa cantidad va
//a haber un pedazo contiguo de 104 (por ser las paginas de
//4 Kb). Pero hay que determinar que pedazo es contiguo.
//Para eso sirven las siguientes dos funciones.
//Son dos separadas porque no queremos perder memoria: cuando
//matemos un proceso vamos a liberar su tss tambien.
static void* new_tss_space(void)
{
    return kmalloc(2*sizeof(tss)+1);
}

static tss* get_contiguous_tss(void* space)
{
    uint pas  = physical_address(current_directory,(intptr)space);
    uint panb = physical_address(
        current_directory,(intptr)space+sizeof(tss));
    uint fas = ALIGN(pas), fanb = ALIGN(panb);
    if(fas == fanb || fas == fanb+1) {
        return space;
    }
    return (void*)((intptr)space+sizeof(tss));
}

//Crea una nueva pila que tiene su tope
//en la direccion dada por KSTACK_START (la pila crece al
//reves en intel).
static int
create_new_stack(page_directory* new_dir, tss* new_tss, bool user)
{
    uint stack_start = (user) ? USTACK_START : KSTACK_START;
    uint stack_pages = (user) ? USTACK_PAGES : KSTACK_PAGES;
    uint sp = stack_start;
    uint flags = PAGEF_P | PAGEF_RW;
    if(user) {
        flags |= PAGEF_U;
    }
    for(uint i = 0; i < stack_pages; i++) {
        map_page(new_dir,sp,frame_alloc(),flags);
        sp += PAGE_SZ;
    }
    if(user) {
        new_tss->esp = USTACK_BASE;
    } else {
        new_tss->esp0 = KSTACK_BASE;
    }
    return 0;
}
#define create_kernel_stack(a,b) create_new_stack(a,b,false)
#define create_user_stack(a,b) create_new_stack(a,b,true)

static void set_tss_kernel_mode(tss* new_tss, uint kernel_esp)
{
    new_tss->ss = new_tss->ds = new_tss->es =
    new_tss->fs = new_tss->gs = DATA_SEGMENT_KERNEL;

    new_tss->esp0 = new_tss->esp = kernel_esp;
    new_tss->cs = CODE_SEGMENT_KERNEL;
}

static void set_tss_gregs(tss* new_tss, gen_regs* regs)
{
    new_tss->eax = regs->eax;
    new_tss->ebx = regs->ebx;
    new_tss->ecx = regs->ecx;
    new_tss->edx = regs->edx;
    new_tss->esi = regs->esi;
    new_tss->edi = regs->ebx;
    new_tss->ebp = regs->ebp;
}

static void init_tss(tss* new_tss, gen_regs* regs,
                     uint eip, uint eflags,
                     page_directory* new_dir)
{
    memset(new_tss,0,sizeof(tss));
    new_tss->cs = CODE_SEGMENT_USER;
    new_tss->eflags = eflags;
    if(regs != NULL) {
        set_tss_gregs(new_tss,regs);
    }
    new_tss->ss = new_tss->es = new_tss->ds =
    new_tss->fs = new_tss->gs = DATA_SEGMENT_USER;

    new_tss->eip = eip;
    new_tss->cr3 = new_dir->physical_address;
    new_tss->ss0 = DATA_SEGMENT_KERNEL;
    new_tss->iomap_addr = sizeof(tss);
}

extern void enter_coma(int);
extern void die_on_signal(int);
extern void ignore_signal(int);
static void init_signal_handling(process_info* p)
{
    p->signal_bitmap = p->ignore_bitmap = 0;
    for(int i = 0; i < SIGNALS; i++) {
        p->signal_handlers[i] = die_on_signal;
    }
    p->signal_handlers[SIGSTOP] = enter_coma;
    p->signal_handlers[SIGCONT] = ignore_signal;
}

void init_process_tree_info(process_info* pi,
                            process_info* parent)
{
    pi->pid = last_pid++;
    pi->status = P_AVAILABLE;
    INIT_LIST_HEAD(&pi->children);
    INIT_LIST_HEAD(&pi->process_list);
    INIT_LIST_HEAD(&pi->parent_list);
    INIT_LIST_HEAD(&pi->sem_queue);
    pi->parent = parent;
    if(parent) {
        process_add_child(parent,pi);
    }
    pi->waiting_child = NULL;
}

process_info* create_process_info(process_info* parent,
                                  short tss_index,
                                  void* tss_space,
                                  tss* tssptr,
                                  page_directory* pd,
                                  const char * cwd)
{
    process_info* pi = kmalloc(sizeof(process_info));
    if(pi == NULL) return NULL;

    memset(pi,0,sizeof(process_info));
    init_process_tree_info(pi,parent);

    pi->tss_selector = tss_index;
    pi->tss_space_start = tss_space;
    pi->tss = tssptr;
    pi->remaining_quantum = START_QUANTUM;
    pi->page_dir = pd;
    pi->kernel_mode = !parent || parent->kernel_mode;

    const char * cwdtemp = (cwd) ? cwd : "/";
    strcpy(pi->cwd,cwdtemp);

    memset(pi->fds,0,sizeof(pi->fds));
    init_signal_handling(pi);
    return pi;
}

static void copy_kernel_stack(page_directory* new_dir)
{
    for(uint i = KSTACK_START;
        i < KSTACK_START+KSTACK_SIZE;
        i += PAGE_SZ) {
        copy_frame(physical_address(new_dir,i),
                   physical_address(current_directory,i));
    }
}

int do_fork(intptr kernel_esp, gen_regs regs, uint32 eflags, intptr eip)
{
    page_directory* new_dir = clone_directory(current_directory);
    process_info * curr = get_current_task();

    void* space = new_tss_space();
    if(space == NULL) {
        return ERROUTMEM;
    }
    tss* new_tss = get_contiguous_tss(space);
    if(new_tss == NULL) {
        return ERROUTMEM;
    }
    init_tss(new_tss,&regs,eip,eflags,new_dir);
    if(create_kernel_stack(new_dir,new_tss)) {
        return ERROUTMEM;
    }
    //No hay que copiar la pila de usuario porque
    //copy on write lo va a hacer solito. Pero la de
    //kernel si porque sino el page fault handler
    //no anda
    copy_kernel_stack(new_dir);
    set_tss_kernel_mode(new_tss,kernel_esp);

    short tss_index = gdt_add_tss((intptr)new_tss);
    if(tss_index == -1) return ERRGDTFULL;

    process_info * pi;
    pi = create_process_info(curr,tss_index,space,
                             new_tss,new_dir,curr->cwd);
    if(pi == NULL) return ERROUTMEM;

    copy_file_descriptors(pi,curr);

    add_process(pi);
    new_tss->eax = 0;
    return pi->pid;
}

//Determina que flags tiene que tener la pagina dado el tipo de
//segmento que estamos mirando
uint determine_flags(elf_segment* es)
{
    uint flags = PAGEF_P; //Presente esta seguro
    if(~es->flags & ELF_ATTR_RB) {
        return flags;
    }
    flags |= PAGEF_U; //Si se puede leer seguro es de usuario
    if(es->flags & ELF_ATTR_WB) {
        flags |= PAGEF_RW;
    }
    return flags;
}

//Pega la imagen dada por el archivo elf (en memoria, indicado
//por el buffer pasado por parametro) al mapa de memoria del
//proceso actual.
int elf_overlay_image(elf_file* elf)
{
    //Tiene que ser el directorio actual porque sino cuando
    //intentemos copiar, no vamos a poder porque claro, las
    //paginas no van a estar mapeadas (si fuera otro directorio)
    page_directory* d = current_directory;
    for(uint i = 0; i < elf->header->ph_entry_count; i++) {
        elf_segment* e = elf_get_segment(elf,i);
        if(e == NULL) return ERRREADINGELF;
        if(e->type == ELF_LOAD && e->mem_size > 0) {
            intptr address,start_address;
            address = start_address = e->virtual_address;
            uint remaining = e->mem_size, copied = 0;
            int memsize = e->mem_size;

            for(; address < start_address + memsize;
                address=NEXT_ALIGN(address)) {

                uint new_frame = frame_alloc();
                uint flags = determine_flags(e);
                //Necesitamos copiar primero por eso los flags
                //son esos
                map_page(d, address, new_frame, PAGEF_FULL);
                uint to_copy = NEXT_ALIGN(address) - address;
                if(to_copy > remaining) {
                    to_copy = remaining;
                }
                remaining -= to_copy;
                char * ptr = (char *) address;
                memcpy(ptr,e->data+copied,to_copy);
                copied += to_copy;
                //Ahora que copiamos este cacho de pagina,
                //ponemos los flags correctos
                map_page(d,address, new_frame, flags);
            }
        }
        elf_free_segment(e);
    }
    return 0;
}

static inline int still_running(process_info* p)
{
    return p &&
           p->status == P_RUNNING &&
           p->remaining_quantum > 0;
}

static inline int add_to_round_robin(process_info* p)
{
    return p->status == P_RUNNING;
}

static process_info* search_available_task(void)
{
    process_info* p;
    list_for_each_entry(p,&processes,process_list) {
        if(p->status == P_AVAILABLE &&
           p->remaining_quantum > 0) {
            return p;
        }
    }
    return NULL;
}

process_info* get_current_task()
{
    return current_process;
}

void invalidate_current_task(void)
{
    current_process = NULL;
}

extern void tss_task_switch(short selector);
void perform_task_switch(process_info* next)
{
    if(next == NULL) {
        kernel_panic("Saltando a tarea nula");
    }
    //Definido en task_switch.asm
    set_current_directory(next->page_dir);
    current_process = next;
    switch_kernel_mode();
    tss_task_switch(next->tss_selector);
}

process_info* next_task()
{
    process_info* current_task = get_current_task();
    if(still_running(current_task)) {
        //La tarea tiene que seguir corriendo
        current_task->remaining_quantum--;
        return current_task;
    } else {
        if(current_task) {
            //Si hay tarea actual (no se bloqueo o no exiteo)
            //la agregamos a la cola round robin
            list_move_tail(&current_task->process_list,&processes);
            if(add_to_round_robin(current_task)) {
                set_runnable(current_task);
            }
        }
        process_info * p = search_available_task();
        p->status = P_RUNNING;
        return p;
    }
    return NULL;
}

static short init_kernel_task(void)
{
    memset(&kernel_tss,0,sizeof(tss));
    return gdt_add_tss((intptr)&kernel_tss);
}

static process_info* init_initial_task(void* code_buffer)
{
    elf_file* e = elf_read_exec(code_buffer);
    if(e == NULL) return NULL;
    //No cambiamos el directorio porque vamos a usar el
    //current_dir duplicado y listo (es necesario para
    //elf_overlay_image porque sino no se puede copiar)
    elf_overlay_image(e);
    elf_destroy(e);

    intptr initial_task_tss = (intptr) &initial_task;

    init_tss(&initial_task,
             NULL,
             elf_entry_point(e),
             get_eflags(),
             current_directory);
    if(create_user_stack(current_directory, &initial_task))
        return NULL;
    if(create_kernel_stack(current_directory, &initial_task))
        return NULL;

    short tss_index = gdt_add_tss(initial_task_tss);
    if(tss_index == -1) {
        return NULL;
    }

    //TSS space es nulo porque cuando lo liberemos, no
    //debemos liberar espacio estatico de kernel
    process_info* p = create_process_info(NULL,tss_index,
                                          NULL,&initial_task,
                                          current_directory,"/");
    add_process(p);
    return p;
}

static short kernel_seg;
void jump_to_initial(void* task_code)
{
    kernel_seg = init_kernel_task();
    process_info* init = init_initial_task(task_code);
    if(init == NULL) {
        kernel_panic("Tarea invalida o no se pudo crear\n");
    }
    set_tr(kernel_seg);
    perform_task_switch(init);
}

static void switch_to_next_task(void)
{
    process_info * p = next_task(),
    * current = get_current_task();
    if(p == NULL && current) {
        //No hay nadie mas asi que ponemos a correr la que
        //ya estaba
        set_runnable(current);
        p = current;
    }
    short tss_selector = get_tr();
    if(current == NULL) {
        set_tr(kernel_seg);
        gdt_remove_tss(tss_selector);
    }
    if(p->tss_selector != tss_selector) {
        perform_task_switch(p);
    }
}

static void remove_quantum(process_info* p)
{
    p->remaining_quantum = 0;
}

void scheduler_init()
{
    INIT_LIST_HEAD(&processes);
    current_process = NULL;
}

int do_exit()
{
    process_info* current = get_current_task();
    free_process(current);
    invalidate_current_task();
    switch_to_next_task();
    kernel_panic("Se volvio a ejecutar una tarea que exiteo");
    return ERRIMPOSSIBLE;
}

static uint count_arguments(char ** args)
{
    uint i = 0;
    while(args[i] != NULL) i++;
    return i;
}

static char * push_stack(char * stack_ptr, void * mem,uint count)
{
    char * nptr = stack_ptr - count;
    memcpy(nptr,mem,count);
    return nptr;
}

static char * push_into_user_stack(char * stack_start,
                                   char ** args, uint _count)
{
    char * stack = stack_start;
    uint pos[_count];

    int count = (int) _count;
    for(int i = 0; i < count; i++) {
        char * str = args[i];
        uint bytes = strlen(str)+1;
        if(bytes > EXEC_MAX_ARGSZ) return NULL;
        stack = push_stack(stack,str,bytes);
        pos[i] = (intptr) stack;
    }
    //Pusheamos el arreglo de puntero a los strings anteriores
    for(int i = count-1; i >= 0; i--) {
        stack = push_stack(stack,&pos[i],sizeof(uint));
    }
    //Pusheamos el puntero al arreglo de punteros
    intptr stackpos = (intptr) stack;
    stack = push_stack(stack,&stackpos,sizeof(stackpos));
    //Pusheamos la cantidad de parametros
    stack = push_stack(stack,&_count,sizeof(_count));
    return stack-sizeof(count);
}

void process_filename(char * user_filename, char * filename)
{
    filename[0] = '\0';
    if(user_filename[0] != '/') {
        strcat(filename,"/tasks/");
    }
    strcat(filename,user_filename);
    uint len = strlen(filename);
    if(len < 4 || strcmp(filename+len-4,".run")) {
        strcat(filename,".run");
    }
}

int do_exec(char* user_filename, char** args, int_trace * t,void * ebp)
{
    int fd = 0, error = 0, read = 0, i = 0;
    elf_file * e  = NULL;
    void * mem    = NULL;
    char * stack  = NULL;
    char filename[FS_MAXLEN+1];
    process_info * p = NULL;
    uint argc = 0, size = 0;

    #define check(c,e) if(!(c)) { error = e; goto done; }
    //Vamos a reemplazar la imagen actual con la imagen dada por
    //el archivo filename (con respecto al directorio actual
    //en caso de ser necesario)

    p = get_current_task();

    argc = count_arguments(args);
    check(argc <= EXEC_MAX_ARGC,ERRTOOMANYARGS);

    //Primero hay que levantar el ejecutable de memoria y validar ciertas
    //cosas como por ejemplo que tenga un tamaño aceptable
    process_filename(user_filename,filename);

    fd = do_open(filename,FS_RD);
    check(fd >= 0, ERRINVFILE);
    size = get_file_size(fd);
    check(size < EXEC_MAX_FSIZE,ERRBIGEXEC);

    mem = kmalloc(size);
    read = do_read(fd,size,mem);

    check(!do_close(fd),ERRREAD);
    check(read >= 0, ERRREAD);
    check((uint)read == size,ERRREAD);

    e = elf_read_exec(mem);
    check(e != NULL, ERRNOTELF);

    //Pusheamos los argumentos en la pila del proceso
    //Esto hay que hacerlo antes de liberar las paginas porque potencialmente
    //los punteros estan en la zona de texto
    stack = push_into_user_stack((char *) USTACK_BASE,args,argc);
    check(stack != NULL, ERRARGTOOBIG);
    t->useresp = (intptr) stack;
    *((intptr*)ebp) = USTACK_BASE;

    //Primero hay que liberar todas las paginas que tenga el proceso
    //de las que sea propietario excepto las de la pila de kernel
    //Vamos a marcar los frames de absolutamente todo como libres.
    //Si alguien viene y pide frames puede terminar con nuestros frames.
    //Ergo ahora no nos debe parar NADIE por eso el cli.
    free_process_data_frames(p);

    //Ahora que las paginas de texto estan afuera, vamos a poner las de
    //la imagen elf
    check(!elf_overlay_image(e),ERRREADINGELF);
    //Cerramos todos los file descriptors abiertos por el proceso
    //excepto entrada y salida estandar por comodidad
    for(i = 2; i < MAX_FDS; i++)
        if(!unused_fd(p,i))
            do_close(i);

    //Asignamos el valor de eip para cuando volvamos de la
    //interrupccion
    t->eip = elf_entry_point(e);
    //Liberamos todos los recursos y devolvemos si hubo error o no
done:
    elf_destroy(e);
    kfree(mem);
    return error;
}

void block_current_process()
{
    process_info* p = get_current_task();
    p->status = P_BLOCKED;
    switch_to_next_task();
}

int do_wait(uint child_pid)
{
    process_info* p = get_current_task();
    process_info* child = get_process_by_pid(child_pid);
    if(child && child->parent == p) {
        p->waiting_child = child;
        block_current_process();
    }
    return 0;
}

int do_sleep()
{
    remove_quantum(get_current_task());
    switch_to_next_task();
    return 0;
}

void wake_up(process_info* p)
{
    if(p->status == P_BLOCKED) {
        set_runnable(p);
    }
}

static void swap_user_stacks(page_directory* p, page_directory* q)
{
    //TODO: Considerar que la pila puede ser de tamaño variable
    //en un futuro cercano
    for(uint i = USTACK_START;
        i < USTACK_START+USTACK_SIZE;
        i += PAGE_SZ) {
        uint pframe = physical_address(p,i);
        uint qframe = physical_address(q,i);
        map_page(p,i,qframe,PAGEF_P | PAGEF_U | PAGEF_RW);
        map_page(q,i,pframe,PAGEF_P | PAGEF_U | PAGEF_RW);
    }
}

static void
set_signal_stack_frame(process_info* p,
                       intptr* prev_esp,
                       intptr* prev_eip,
                       void (*handler)(int),
                       int signal_code)
{
    int* user_stack = (int*)(*prev_esp);
    //Necesitamos tener la pila del proceso mapeada.
    //Para eso vamos a temporalmente swappear pilas
    process_info* ct = get_current_task();
    swap_user_stacks(ct->page_dir,p->page_dir);
    *(--user_stack) = *prev_eip;
    *(--user_stack) = (intptr) handler;
    *(--user_stack) = signal_code;
    *prev_esp = (intptr) user_stack;
    swap_user_stacks(ct->page_dir,p->page_dir);
}

extern void signal_trampoline(void);
void prepare_handler(process_info* p,intptr * prev_esp,
                     intptr* prev_eip, int signal_code)
{
    set_signal_stack_frame(p,prev_esp,prev_eip,
                           p->signal_handlers[signal_code],
                           signal_code);
    p->ignore_bitmap |= (1 << signal_code);
    *prev_eip = (intptr) signal_trampoline;
}

static void
handle_signals_kernel_mode(process_info* p,
                           intptr * user_esp,
                           intptr * user_eip)
{
    int i;
    for(i = SIGNALS-1; i >= 0; i--) {
        int sigcode = 1 << i;
        if(p->signal_bitmap & sigcode) {
            prepare_handler(p,user_esp,user_eip,i);
            p->signal_bitmap &= ~sigcode;
        }
    }
}

static void handle_sigcont(process_info * p){
    if(p->signal_bitmap & (1 << SIGSTOP)) {
        p->signal_bitmap &= ~(1 << SIGSTOP);
        p->signal_bitmap &= ~(1 << SIGCONT);
    }
}

int do_kill(int pid, int signal)
{
    process_info* p = get_process_by_pid(pid);
    if(p == NULL) {
        return ERRINVPID;
    }
    if(signal >= SIGNALS || signal < 0) {
        return ERRINVSIG;
    }
    if(p->ignore_bitmap & (1 << signal)) {
        return ERRIGNSIG;
    }
    if(p->status == P_COMA) {
        set_runnable(p);
    }
    if(p->kernel_mode) {
        p->signal_bitmap |= (1 << signal);
        if(signal == SIGCONT)
            handle_sigcont(p);
    } else {
        prepare_handler(p,&p->tss->esp,
                        &p->tss->eip,signal);
    }
    return 0;
}

static bool overridable_signal(int signal)
{
    switch(signal) {
    case SIGKILL:
    case SIGSTOP:
        return false;
    default:
        return true;
    }
}

int do_signal(int signal, signal_handler handler)
{
    if(signal > 0 && signal < SIGNALS &&
       overridable_signal(signal)) {
        process_info* p = get_current_task();
        if(p == NULL) {
            return ERRINVPID;
        }
        p->signal_handlers[signal] = handler;
        return 0;
    }
    return ERRINVSIG;
}

int do_coma()
{
    process_info* p = get_current_task();
    if(p == NULL) return -ERRINVPID;
    p->status = P_COMA;
    p->ignore_bitmap &= ~(1 << SIGSTOP);
    return do_sleep();

}

int do_clear_signal(int signal)
{
    process_info* p = get_current_task();
    if(p->ignore_bitmap & (1 << signal)) {
        p->ignore_bitmap &= ~(1 << signal);
        return 0;
    }
    return -1;
}

void do_get_cwd(char * buf)
{
    process_info * p = get_current_task();
    if(p == NULL) kernel_panic("Proceso inactivo");
    buf[0] = '\0';
    strcpy(buf,p->cwd);
}

int do_set_cwd(const char * new_cwd)
{
    process_info * p = get_current_task();
    if(p == NULL) kernel_panic("Proceso inactivo");
    int error = invalid_path(new_cwd);
    if(!error) {
        char * ptr = p->cwd;
        if(new_cwd[0] != '/') {
            while(*ptr != '\0')
                ptr++;
            if(*(ptr-1) != '/')
                *ptr++ = '/';
        }
        strcpy(ptr,new_cwd);
    }
    return error;
}

int do_get_pid()
{
    process_info * p = get_current_task();
    if(p == NULL) kernel_panic("Proceso inactivo");
    return p->pid;
}
