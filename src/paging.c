#include <paging.h>
#include <memory.h>
#include <frames.h>
#include <exception.h>
#include <utils.h>
#include <scrn.h>
#include <irq.h>
#include <tasks.h>
#include <proc.h>

#define PAGE_BW 12

//Heap de kernel. Eventualmente habra una de usuario para los
//procesos. De aca saca memoria el kernel
static kmem_map_header* kernel_heap = NULL;

#define KHEAP_START     0xC0000000
#define KHEAP_INISIZE   0x400000
#define KHEAP_MAXIMUM   0xDFFFF000

//Cantidad de tablas que definimos inicialmente. Es la solucion
//mas sencilla para el problema de necesitar tablas de pagina que
//mapear al directorio del kernel cuando appendeamos memoria a la
//heap del kernel: las asignamos previamente y esperemos que alcanzen.
#define TABLES_ID_MAP   8

page_directory* current_directory, * kernel_dir;

kmem_map_header * get_kernel_heap(){
    return kernel_heap;
}

//Devuelve la direccion fisica correspondiente a una
//direccion virtual dado el directorio de paginas.
//PRE: Las tablas del directorio deben estar mapeadas
//(Considerando que esto corre en modo kernel y que la
//administracion de paginas es propiedad del mismo
//no me parece falto de razon)
uint physical_address(page_directory* pd, uint va)
{
    uint pdi = PAGE_DIR(va),
         pti = PAGE_TABLE(va),
         offset = PAGE_OFFSET(va);
    if(!pd->tables_phys[pdi].present) {
        return (uint) -1;
    }
    page_table* p = pd->tables_virtual[pdi];
    if(!p->entries[pti].present) {
        return (uint) -1;
    }
    return (p->entries[pti].frame << PAGE_BW) + offset;
}

void set_current_directory(page_directory* dir)
{
    current_directory = dir;
}

//Habilita paginacion activando dir como el directorio actual
void switch_page_directory(page_directory* dir)
{
    uint eflags = irq_cli();
    set_current_directory(dir);
    __asm__ __volatile__ ("mov %0, %%cr3" :: "r" (dir->physical_address));
    uint cr0;
    __asm__ __volatile__ ("mov %%cr0, %0" : "=r" (cr0));
    cr0 |= PAGING_ENABLED_MASK | WP_MASK;
    //para que tire page fault si el
    //kernel trata de acceder a un lugar
    //de solo lectura
    __asm__ __volatile__ ("mov %0, %%cr0" :: "r" (cr0));
    irq_sti(eflags);
}

//Siguiente posicion para pedir una tabla de paginas
static intptr page_tables_vs;

//Inicializa el administrador de frames (internamente usa un bitmap)
static uint frames_init(intptr start_address, intptr memory_end)
{
    uint used_upper_mem = 0, megabyte = 1024*1024;
    uint total_upper_mem = memory_end - start_address;
    if(start_address > megabyte) {
        used_upper_mem = start_address - megabyte;
    }
    uint available_frames = (total_upper_mem - used_upper_mem)/ 0x1000;
    return frame_alloc_init((void*) start_address,available_frames);
}

//Mapea table_index a un frame (que se espera sea consecutivo porque esta
//funcion se corre al principio para generar el mapa de memoria de kernel previo
//a activar paginacion, porque despues se utiliza la heap para eso).
void create_table_entry(page_directory* kernel_directory, uint table_index)
{
    intptr table_dir = frame_alloc(); //Como esto corre al principio,
    //frame_alloc devuelve frames consecutivos.
    if(table_dir != page_tables_vs) {
        kernel_panic("Paginas de kernel no continuas\n");
    }
    page_table_entry table_entry;
    table_entry.present = 1;
    table_entry.read_write = 1;
    table_entry.user = 1; //Proteccion a nivel tabla no tiene sentido
    table_entry.frame = table_dir >> 12;
    page_tables_vs += PAGE_SZ;
    kernel_directory->tables_phys[table_index] = table_entry;
    kernel_directory->tables_virtual[table_index] = (page_table*) table_dir;
}

//Extiende el mapa de memoria de kernel pasado por parametro, indicando
//la cantidad de paginas que deseamos agregar al final.
void* paging_append_core(kmem_map_header* mh, uint pages)
{
    void* prev = (void*) mh->heap_end;
    while(pages-- > 0) {
        if(mh->heap_end >= KHEAP_MAXIMUM) {
            kernel_panic("Heap excedida de tamaño");
        }
        uint frame = frame_alloc();
        //La tabla siempre existe pues la preasignamos antes de empezar.
        //Gastamos memoria pero nos aseguramos que todos los procesos van
        //a poder ver la heap de kernel sin mayores problemas
        map_page(kernel_dir,mh->heap_end, frame, PAGEF_P | PAGEF_RW);
        mh->heap_end += PAGE_SZ;
    }
    return prev;
}

static page_table* clone_table(page_table* src)
{
    page_table* pt;
    pt = kmem_alloc_aligned(kernel_heap,sizeof(page_table));
    if(pt == NULL) {
        return NULL;
    }
    memset(pt,0,sizeof(page_table));
    page_entry* srcpg = src->entries, * dstpg = pt->entries;
    for(uint i = 0; i < PAGE_FRAMES; i++) {
        if(!srcpg[i].present || !srcpg[i].user) {
            continue;
        }
        memcpy(&dstpg[i],&srcpg[i],sizeof(page_entry));
        dstpg[i].read_write = 0;
        if(srcpg[i].read_write) {
            srcpg[i].read_write = 0;
            srcpg[i].copy_on_write = 1;
            dstpg[i].copy_on_write = 1;
        }
        frame_add_alias(srcpg[i].frame << 12);
    }
    return pt;
}

//Clona un directorio: Se usa para forkear.
page_directory* clone_directory(page_directory* src)
{
    page_directory* p;
    p = kmem_alloc_aligned(kernel_heap,sizeof(page_directory));
    if(p == NULL) {
        return NULL;
    }
    memset(p,0,sizeof(page_directory));
    p->physical_address = physical_address(kernel_dir,(intptr)p);
    for(uint i = 0; i < PAGE_TABLES; i++) {
        if(!src->tables_phys[i].present) {
            continue;
        }
        if(kernel_dir->tables_virtual[i] == src->tables_virtual[i]) {
            //Pagina de kernel: No queremos copiarla, queremos linkearla
            p->tables_phys[i] = src->tables_phys[i];
            p->tables_virtual[i] = src->tables_virtual[i];
        } else {
            //Pagina de usuario: Ahora si que queremos copiarla
            p->tables_virtual[i] = clone_table(src->tables_virtual[i]);
            p->tables_phys[i].frame =
                physical_address(kernel_dir,
                                 (intptr)p->tables_virtual[i]) >> PAGE_BW;
            p->tables_phys[i].present = 1;
            p->tables_phys[i].read_write = 1;
            //Podemos tener tablas que sean de kernel:
            //Potencialmente las de la pila de kernel
            p->tables_phys[i].user = src->tables_phys[i].user;
        }
    }
    return p;
}

extern uint signal_handlers_start;
extern uint signal_handlers_end;

//Inicializa paginacion dadas la ultima direccion del codigo del kernel y
//la ultima direccion fisica dada por la memoria
void paging_init(intptr end_address, intptr memory_end)
{
    end_address = NEXT_ALIGN(end_address);
    kernel_dir = (page_directory*) end_address;
    memset(kernel_dir,0,sizeof(page_directory));
    kernel_dir->physical_address = (intptr) kernel_dir;
    end_address += sizeof(page_directory);
    end_address = NEXT_ALIGN(end_address);
    //Creamos el manejador para los frames.
    end_address = frames_init(end_address,memory_end);
    //Seteamos el comienzo de las tablas en page_tables_vs
    page_tables_vs = end_address;
    uint page;
    //Asignamos las tablas necesarias para lo que tenemos de
    //kernel hasta ahora.
    for(page = 0; page < end_address; page += PAGE_SZ) {
        if(!kernel_dir->tables_virtual[PAGE_DIR(page)]) {
            create_table_entry(kernel_dir,PAGE_DIR(page));
        }
    }
    //Asignamos las tablas necesarias para todas las paginas de la heap.
    //Gastamos algo de memoria pero nos aseguramos que se va a poder ver todo.
    for(page = KHEAP_START; page < KHEAP_MAXIMUM; page += PAGE_SZ) {
        if(!kernel_dir->tables_virtual[PAGE_DIR(page)]) {
            create_table_entry(kernel_dir,PAGE_DIR(page));
        }
    }
    //Mapeo un cierto numero de entradas de manera fija asi no tenemos dramas
    //de mapear las tablas para direcciones (con mapear 4 tablas tenemos
    //mapeados las tablas para los primeros 8*4M = 32 MB de espacio,
    //mas que suficiente para asignar frames de tablas de paginas para heap.
    for(uint table = 0; table < TABLES_ID_MAP; table++) {
        if(!kernel_dir->tables_virtual[table]) {
            create_table_entry(kernel_dir,table);
        }
    }
    end_address = page_tables_vs;
    //Hacemos identity mapping del kernel: Todo lo necesario es ahora
    //accesible de manera transparente.
    for(page = 0; page < end_address; page += PAGE_SZ) {
        map_page(kernel_dir,page,page,PAGEF_P | PAGEF_RW);
    }
    //Los signal handlers son de usuario asi que los mapeamos como
    //usuario. Usamos simbolos de linker para encontrarlos
    for(page = (intptr) &signal_handlers_start;
        page < (intptr) &signal_handlers_end; page += PAGE_SZ) {
        map_page(kernel_dir,page,page,PAGEF_P | PAGEF_U);
    }
    //Ahora mapeamos la heap inicial de kernel
    for(page = KHEAP_START; page < KHEAP_START+KHEAP_INISIZE; page += PAGE_SZ) {
        uint frame = frame_alloc();
        map_page(kernel_dir,page,frame,PAGEF_P | PAGEF_RW);
    }
    register_exception_handler(page_fault_handler,14);
    switch_page_directory(kernel_dir);
    kernel_heap = kmem_init((void*)KHEAP_START,KHEAP_INISIZE);
    current_directory = clone_directory(kernel_dir);
    switch_page_directory(current_directory);
}

static void tlb_flush(void)
{
    uint eflags = irq_cli();
    uint cr0;
    __asm__ __volatile__ ("mov %%cr0, %0" : "=r" (cr0));
    if(cr0 & PAGING_ENABLED_MASK) {
        uint cr3;
        //Si paginacion esta habilitada, entonces
        //cambiamos el cr3 para que se flushee la tlb
        __asm__ __volatile__ ("mov %%cr3, %0" : "=r" (cr3));
        __asm__ __volatile__ ("mov %0, %%cr3" :: "r" (cr3));
    }
    irq_sti(eflags);
}

void clear_page_entry(page_directory * pd, uint pdi, uint pti)
{
    page_entry * pe = &pd->tables_virtual[pdi]->entries[pti];
    frame_free(pe->frame << 12);
}

void clear_table_entry(page_directory * pd, uint entry){
    kmem_free(kernel_heap,pd->tables_virtual[entry]);
    pd->tables_virtual[entry] = NULL;
}

//Mapea una pagina, posiblemente consiguiendo espacio de kernel
//para una tabla de paginas. La podemos usar libremente incluso
//antes de tener heap de kernel porque preasignamos (para eso
//esta la guarda de kernel panic si la heap no esta cargada)
void map_page(page_directory* pd, uint va,uint pa,uint flags)
{
    //No nos fijamos que va este alineada a pagina.
    //Esto es util porque si queremos mapear una
    //direccion offseteada en la pagina, tenemos
    //que en realidad mapear toda la pagina listo
    uint pdi = PAGE_DIR(va),
         pti = PAGE_TABLE(va);
    if(!pd->tables_virtual[pdi]) {
        if(!kernel_heap) {
            kernel_panic("La heap del kernel no esta activa");
        }
        void* page = kmem_alloc_aligned(kernel_heap,PAGE_SZ);
        if(page == NULL) {
            kernel_panic("No hay mas memoria para tablas\n");
        }
        uint frame = physical_address(kernel_dir,(intptr) page);
        memset(page,0,PAGE_SZ);
        page_table_entry tentry;
        memset(&tentry,0,sizeof(tentry));
        tentry.frame = frame >> 12;
        tentry.present = 1;
        tentry.read_write = 1;
        //Paginacion a nivel de tabla no tiene sentido
        //considerando que se refuerza a nivel pagina
        tentry.user = 1;
        pd->tables_phys[pdi] = tentry;
        pd->tables_virtual[pdi] = page;
    }
    page_entry* p = &pd->tables_virtual[pdi]->entries[pti];
    p->frame = pa >> PAGE_BW;
    p->present = (flags & PAGEF_P) ? 1 : 0;
    p->read_write = (flags & PAGEF_RW) ? 1 : 0,
    p->user = (flags & PAGEF_U) ? 1 : 0;
    tlb_flush(); //Flusheamos toda la TLB (ineficiente pero correcto)
}

//Liberar todo un directorio de paginas
void page_directory_destroy(page_directory * p)
{
    for(uint pdi = 0; pdi < PAGE_TABLES; pdi++) {
        page_table* pt = p->tables_virtual[pdi];
        if(!pt || pt == kernel_dir->tables_virtual[pdi])
            continue;
        for(uint pti = 0; pti < PAGE_FRAMES; pti++) {
            page_entry* pe = &pt->entries[pti];
            if(!pe->present) continue;

            frame_free(pe->frame << 12);
        }
        clear_table_entry(p,pdi);
    }
    kmem_free(kernel_heap,p);
}

static void page_error_kill(const char* message, exception_trace* t)
{
    scrn_cls();
    scrn_setcursor(0,0);
    uint errorc = t->error_code;
    uint page = t->ctrace.cr2;
    uint was_present = errorc & 0x1;
    uint was_write = errorc & 0x2;
    uint was_user = errorc & 0x4;
    uint overwrite = errorc & 0x8;
    scrn_printf("PAGE FAULT: %s\n",message);
    scrn_printf("\tPagina: %u Error code: %u\n",page,t->error_code);
    scrn_printf("\tPagina presente? %b\n",was_present);
    scrn_printf("\tEn escritura? %b\n",was_write);
    scrn_printf("\tModo usuario? %b\n",was_user);
    scrn_printf("\tSobreescritura? %b\n",overwrite);
    scrn_printf("\tTSS Selector: %u\n\n", get_tr());
    scrn_printf("EIP = %u, EFLAGS = %u, UESP = %u ESP = %u\n",
                t->itrace.eip,t->itrace.eflags,t->itrace.useresp, t->rtrace.esp);
    while(1) {
        ;
    }
}

void do_copy_on_write(uint page, exception_trace* t)
{
    uint pdi = PAGE_DIR(page), pti = PAGE_TABLE(page);
    page_entry* p =
        &current_directory->tables_virtual[pdi]->entries[pti];
    if(!p->copy_on_write) {
        page_error_kill("WRITE EN PAGINA DE SOLO LECTURA",t);
    }
    uint original = p->frame << 12;
    uint new = frame_alloc();
    copy_frame(new,original);
    p->frame = new >> 12;
    p->copy_on_write = 0;
    p->read_write = 1;
}

void page_fault_handler(exception_trace t)
{
    //No queremos que nos interrumpan: Esto modifica paginas
    //de kernel y esas cosas.
    uint eflags = irq_cli();
    uint errorc = t.error_code;
    uint page = t.ctrace.cr2;
    uint was_present = errorc & 0x1;
    uint was_write = errorc & 0x2;
    uint was_user = errorc & 0x4;
    uint overwrite = errorc & 0x8;
    if(was_present && was_write && !overwrite) {
        do_copy_on_write(page,&t);
    } else {
        if(was_user) {
            if(overwrite) {
                page_error_kill("SOBRESCRITOS BITS DE CONTROL",&t);
            }
            if(!was_present) {
                //TODO: Ver de hacer swapping al disco.
                page_error_kill("PAGINA NO PRESENTE",&t);
            } else {
                page_error_kill("ERROR DE PROTECCION EN PAGINACION\n",&t);
            }
            //do_exit();
        } else {
            //TODO: Este es el unico caso en que
            //hay que matar al kernel. En los
            //demas hay que matar al proceso.
            page_error_kill("ERROR EN EL KERNEL",&t);
            while(1) ;
        }
    }
    irq_sti(eflags);
}
