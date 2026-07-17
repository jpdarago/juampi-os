// kernel.c - Startup routine of the operating system kernel.
// It is called from loader.asm

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

// Linker symbol for the end of the kernel. The address it contains is
// a location right after the kernel ends. It is defined in the linker script
extern uint kernel_end;

#ifdef KTEST
// Defined in tests/ktest.c; runs the in-kernel test suite and exits QEMU.
void ktest_main(void);
#endif

void kmain(multiboot_info_t* mbd, unsigned long magic)
{
    if(magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        scrn_setmode(GREEN,BLACK);
        scrn_print("Something went very very wrong. I don't know what else to tell you.");
        return;
    }
    scrn_cls();
    scrn_setmode(GREEN,BLACK);
    scrn_print("WELCOME TO juampiOS\n\t"
               "We are working to bring you "
               "the OS of the future.\n");
    scrn_print("INITIALIZING GDT...");
    gdt_init();
    gdt_flush();
    scrn_print("OK\nINITIALIZING IDT FOR EXCEPTIONS...");
    initialize_exception_handlers();
    idt_init_exceptions();
    remap_pic();
    scrn_print("OK\nINITIALIZING IDT FOR INTERRUPTS AND SYSCALLS...");
    irq_init_handlers();
    syscalls_initialize();
    idt_init_interrupts();
    idt_init_syscalls();
    idt_flush();
    irq_sti_force();

    scrn_printf("OK\nCHECKING MODULE STATE...");
    scrn_printf("%u MODULES LOADED\n",mbd->mods_count);

    scrn_print("CHECKING MEMORY STATE\n");
    // We check the amount of RAM memory present.
    if(mbd->flags & 1) {
        scrn_printf("\tAmount of RAM in the system:\n"
                    "\t\tLower: %u Kb, Upper: %u Kb\n",
                    mbd->mem_lower,mbd->mem_upper);
    } else {
        kernel_panic("Invalid GRUB memory map");
    }
    scrn_print("INITIALIZING KERNEL MEMORY STRUCTURES...");
#ifdef KTEST
    // Test builds boot via `qemu -kernel` with no modules, so take the
    // end-of-kernel address from the linker symbol rather than a GRUB module.
    uint kernel_end_addr = (uint) &kernel_end;
#else
    module_t* grub_modules = (module_t*) mbd->mods_addr;
    uint kernel_end_addr = grub_modules[mbd->mods_count-1].mod_end;
#endif
    // The upper memory map starts from the first megabyte, ergo the first
    // location where we go past the end is 1024 kilobytes plus the memory GRUB reports
    paging_init(kernel_end_addr, (1024+mbd->mem_upper)*1024);
    scrn_printf("OK\n");

#ifdef KTEST
    // Memory subsystems are up. Run the integration tests and exit QEMU. We
    // stop before disk/FS/scheduler init, which need hardware and modules the
    // test VM does not provide.
    ktest_main();
    while(1) ;
#else
    scrn_print("INITIALIZING ATA DISK\n");
    hdd_init();
    scrn_printf("INITIALIZING MINIX FILESYSTEM\n");
    init_disk_super_block();
    keybuffer_init(1024);
    scheduler_init();

    void * buffer = (void *) grub_modules[0].mod_start;
    jump_to_initial(buffer);

    while(1) ;
#endif
}
