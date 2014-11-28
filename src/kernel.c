//kernel.c - Rutina de inicio del kernel del sistema operativo.
//Es llamada desde loader.asm

#include <utils.h>
#include <gdt.h>
#include <scrn.h>
#include <idt.h>
#include <multiboot.h>
#include <irq.h>
#include <memory.h>
#include <hdd.h>
#include <timer.h>
#include <frames.h>
#include <exception.h>
#include <paging.h>
#include <elf.h>
#include <tasks.h>
#include <syscalls.h>
#include <vfs.h>
#include <fs_minix.h>
#include <asserts.h>
#include <keyboard.h>
#include <cmos.h>

//Simbolo de linker para el final del kernel. La direccion que contiene es
//un lugar despues de que termina el kernel. Esta definido en en linker script
extern uint kernel_end;

void kmain(multiboot_info_t* mbd, unsigned long magic)
{
	if(magic != MULTIBOOT_BOOTLOADER_MAGIC) {
		scrn_setmode(GREEN,BLACK);
		scrn_print("Algo salio muy muy mal. No se que mas decirte.");
		return;
	}
	scrn_cls();
	scrn_setmode(GREEN,BLACK);
	scrn_print("BIENVENIDO A juampiOS\n\t"
	           "Estamos trabajando para ofrecerle "
	           "el OS del futuro.\n");
	scrn_print("INICIALIZANDO GDT...");
	gdt_init();
	gdt_flush();
	scrn_print("OK\nINICIALIZANDO IDT PARA LAS EXCEPCIONES...");
	initialize_exception_handlers();
	idt_init_exceptions();
	remap_pic();
	scrn_print("OK\nINICIALIZANDO IDT PARA LAS INTERRUPCIONES Y SYSCALLS...");
	irq_init_handlers();
	syscalls_initialize();
	idt_init_interrupts();
	idt_init_syscalls();
	idt_flush();
	irq_sti_force();
	/*
	//Test de system call
	test_print_syscall();
	while(1);
	*/
	scrn_printf("OK\nCHEQUEANDO ESTADO DE LOS MODULOS...");
	scrn_printf("%u MODULOS CARGADOS\n",mbd->mods_count);
	/*
	    scrn_printf("La tarea inicial empieza en %u y termina en %u\n",it->mod_start,it->mod_end);
	*/
	scrn_print("CHECKEANDO ESTADO DE LA MEMORIA\n");
	//Chequeamos que la cantidad de memoria RAM presente.
	if(mbd->flags & 1) {
		scrn_printf("\tCantidad de RAM en el sistema:\n"
		            "\t\tLower: %u Kb, Upper: %u Kb\n",
		            mbd->mem_lower,mbd->mem_upper);
	} else {
		kernel_panic("Mapa de memoria de GRUB invalido");
	}
	scrn_print("INICIALIZANDO LAS ESTRUCTURAS DE MEMORIA DEL KERNEL...");
	module_t* grub_modules = (module_t*) mbd->mods_addr;
	uint kernel_end_addr = grub_modules[mbd->mods_count-1].mod_end;
	//El mapa de memoria upper es a partir del primer megabyte ergo el primer
	//lugar donde nos vamos de largo es 1024 kilobytes mas la memoria que dice GRUB
	paging_init(kernel_end_addr, (1024+mbd->mem_upper)*1024);
	scrn_printf("OK\n");
	/*
	    //Test de appendeo de core al heap

	    void * lotsofmem;uint size = 0x400000;
	    scrn_printf("Antes la heap iba hasta %u\n",kernel_heap->heap_end);
	    lotsofmem = kmem_alloc(kernel_heap,size+1);
	    scrn_printf("Ahora la heap va hasta %u\n",kernel_heap->heap_end);

	    //Test de asignacion de memoria
	    uint * some_mem = kmem_alloc(kernel_heap,4*sizeof(uint));
	    scrn_printf("Malloc devolvio la direccion: %u\n",(uint)some_mem);
	    for(int i = 0; i < 4; i++) some_mem[i] = i;
	    scrn_printf("Los valores: %u %u %u %u\n",some_mem[0],some_mem[1],some_mem[2],some_mem[3]);
	    kmem_free(kernel_heap,some_mem);

	    //Test de page fault handler basico
	    uint * not_mapped = (uint *) 0xDEADBEEF;
	    *not_mapped = 0xBADC0DE;

	    //Test de obtencion de physical address
	    void * mem = kmem_alloc(kernel_heap,128);
	    uint address = physical_address(current_directory,(uint)mem);
	    scrn_printf("La direccion fisica para %u es %u\n",(uint)mem,address);

	    //Test de copy on write
	    map_page(current_directory,0xDEADC0DE,frame_alloc(),PAGEF_P | PAGEF_U | PAGEF_RW);

	    uint * addr = (uint *) 0xDEADC0DE;
	    *addr = 0xBADC0DE;
	    *(addr+4) = 0x600DC0DE;
	    scrn_printf("Antes valia %u\n",*addr);
	    page_directory * new_directory = clone_directory(current_directory),
	                   * previous_directory = current_directory;
	    switch_page_directory(new_directory);
	    scrn_printf("Escribiendo a la direccion %u\n",(uint)addr);
	    *addr = 0xBADBEEF;
	    switch_page_directory(previous_directory);
	    scrn_printf("Ahora esa posicion vale: %u y la siguiente %u\n",*addr,*(addr+4));

	    //Test de poner un elf como overlay

	    void * task_start = grub_modules[0].mod_start;
	    elf_file * e = elf_read_exec(task_start);
	    elf_overlay_image(e);
	    elf_destroy(e);

	    //Test de TSS
	    scrn_print("Agregando una entrada en la gdt para tss\n");
	    scrn_printf("Obtuvimos el selector %u\n",gdt_add_tss(0xDEADBEEF));

	    //Test de ELF
	    scrn_printf("Leyendo el ELF header a ver si anda");
	    elf_file * e = elf_read_exec(it->mod_start);
	    scrn_printf("El punto de entrada de este ELF es %u\n",elf_entry_point(e));

	    //Test de inicializar una nueva tarea
	*/
	
	scrn_print("INICIALIZANDO DISCO ATA\n");
	hdd_init();
/*	uchar data[2*ATA_SECTSIZE];
	hdd_read(1,2,data);
	char * data3 = "Hola como te va juampa";
	memcpy(&data[ATA_SECTSIZE-1],data3,strlen(data3)+1);
	hdd_write(1,2,data);
	while(1);	
*/
	scrn_printf("INICIALIZANDO FILESYSTEM MINIX\n");
	init_disk_super_block();
/*	super_block * b = get_disk_super_block();

	inode* root = b->root;
	file_object f = { .access_mode = FS_RD };
	inode* tareas = root->i_ops->lookup(root,"tasks");
	fail_unless(tareas != NULL);
	inode* prueba1 = root->i_ops->lookup(tareas,"shell.run");

	fail_if(prueba1 == NULL);
	fail_unless(prueba1->inode_type == FS_FILE);

	scrn_printf("CONSIGUIENDO CODIGO TAREA\n");

	root->f_ops->open(prueba1,&f);
	char * buffer = kmalloc(prueba1->file_size+1);
	fail_if(buffer == NULL);

	int read = f.f_ops->read(&f,prueba1->file_size,buffer);
	fail_unless(read == prueba1->file_size);
	buffer[read] = '\0';
*/
	keybuffer_init(1024);
/*	scrn_cls();
	
	for(;;){
		char c = keybuffer_consume();
		if(c == -1) continue;
		scrn_putc(c,scrn_getmode());
	}
*/
/*	scrn_printf("CONSIGUIENDO FECHA Y HORA\n");
	date d;

	while(1){
		get_current_date(&d);
		scrn_printf("Hoy es %d/%d/%d %d:%d:%d\r",
					d.day,d.month,d.year,
					d.hour,d.minute,d.second);
	}
*/	
	scheduler_init();

	void * buffer = (void *) grub_modules[0].mod_start;
	jump_to_initial(buffer);

	while(1);
}
