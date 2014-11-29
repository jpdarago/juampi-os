// kernel.c - Rutina de inicio del kernel del sistema operativo.
// Es llamada desde loader.asm

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

// Simbolo de linker para el final del kernel. La direccion que contiene es
// un lugar despues de que termina el kernel. Esta definido en en linker script
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

    scrn_printf("OK\nCHEQUEANDO ESTADO DE LOS MODULOS...");
    scrn_printf("%u MODULOS CARGADOS\n",mbd->mods_count);

    scrn_print("CHECKEANDO ESTADO DE LA MEMORIA\n");
    // Chequeamos que la cantidad de memoria RAM presente.
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
    // El mapa de memoria upper es a partir del primer megabyte ergo el primer
    // lugar donde nos vamos de largo es 1024 kilobytes mas la memoria que dice GRUB
    paging_init(kernel_end_addr, (1024+mbd->mem_upper)*1024);
    scrn_printf("OK\n");

    scrn_print("INICIALIZANDO DISCO ATA\n");
    hdd_init();
    scrn_printf("INICIALIZANDO FILESYSTEM MINIX\n");
    init_disk_super_block();
    keybuffer_init(1024);
    scheduler_init();

    void * buffer = (void *) grub_modules[0].mod_start;
    jump_to_initial(buffer);

    while(1) ;
}
