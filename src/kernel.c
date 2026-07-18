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
#include <asserts.h>
#include <keyboard.h>
#include <cmos.h>
#include <serial.h>
#include <limine.h>

// Linker symbol for the end of the kernel. The address it contains is
// a location right after the kernel ends. It is defined in the linker script
extern uint kernel_end;

// --- Limine boot protocol ---------------------------------------------------
// The kernel is booted by Limine (see docs/x86-64-port.md), which hands us a
// 64-bit long-mode environment with a higher-half direct map already set up.
// Requests are placed in the .limine_requests section (kept by the linker
// script) and answered by the bootloader before it jumps to kmain.
__attribute__((
        used,
        section(".limine_requests"))) static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".limine_requests"))) static volatile struct
        limine_memmap_request memmap_request = {.id = LIMINE_MEMMAP_REQUEST,
                                                .revision = 0};

__attribute__((
        used,
        section(".limine_requests"))) static volatile struct limine_hhdm_request
        hhdm_request = {.id = LIMINE_HHDM_REQUEST, .revision = 0};

// Section markers that delimit the request list for the bootloader's scan.
__attribute__((used,
               section(".limine_requests_"
                       "start"))) static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used,
               section(".limine_requests_"
                       "end"))) static volatile LIMINE_REQUESTS_END_MARKER;

// Minimal panic for the early boot path: print to serial and halt.
static void early_halt(const char* msg)
{
    serial_print(msg);
    while (1) {
        __asm__ __volatile__("hlt");
    }
}

// Non-fatal breakpoint (int3) handler used by the milestone-2 self-test.
static volatile int bp_hits;
static void breakpoint_handler(interrupt_frame* f)
{
    (void)f;
    bp_hits++;
}

#ifdef KTEST
// Defined in tests/ktest.c; runs the in-kernel test suite and exits QEMU.
void ktest_main(void);
#endif

// kmain is the ELF entry point; Limine jumps here in 64-bit long mode.
void kmain(void)
{
    // --- x86-64 port, milestone 0 -------------------------------------------
    // The 32-bit subsystems below (GDT, IDT, paging, disk, scheduler) are being
    // ported to long mode one milestone at a time; until then they are compiled
    // out. This minimal entry proves Limine booted us into 64-bit long mode and
    // that the boot protocol requests were answered.
    serial_init();
    serial_print("\n=== juampiOS booting (COM1 serial console) ===\n");
    serial_print("juampiOS: running in 64-bit long mode (booted by Limine)\n");

    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        early_halt("juampiOS: PANIC - Limine base revision unsupported\n");
    }
    if (memmap_request.response == NULL || hhdm_request.response == NULL) {
        early_halt("juampiOS: PANIC - Limine did not answer boot requests\n");
    }

    // Prove the protocol works end to end: report the higher-half offset and
    // the usable-RAM total the bootloader gave us.
    serial_print("juampiOS: Limine boot protocol OK\n");
    uint64_t usable = 0;
    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry* e = memmap_request.response->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            usable += e->length;
        }
    }
    serial_print("juampiOS: HHDM offset=");
    serial_hex(hhdm_request.response->offset);
    serial_print(", memmap entries=");
    serial_dec(memmap_request.response->entry_count);
    serial_print(", usable RAM=");
    serial_dec(usable / (1024 * 1024));
    serial_print(" MiB\n");

    // --- Milestone 1: frame allocator + 4-level paging + kernel heap --------
    // Use the largest usable region Limine reported as the physical frame pool.
    uintptr best_base = 0, best_len = 0;
    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry* e = memmap_request.response->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length > best_len) {
            best_base = e->base;
            best_len = e->length;
        }
    }
    paging_init(hhdm_request.response->offset, best_base, best_len);

    // Self-test: distinct frames, a writable kernel-heap block, and a fresh
    // 4-level mapping that round-trips a value and resolves back to its frame.
    uintptr free_before = frames_available();
    uintptr f1 = frame_alloc();
    uintptr f2 = frame_alloc();
    int* h = kmalloc(64);
    h[0] = 0x1234;
    h[15] = 0x5678;

    uintptr scratch_va = 0xffffd00000000000ull;
    uintptr scratch_pa = frame_alloc();
    map_page(kernel_dir, scratch_va, scratch_pa, PAGEF_P | PAGEF_RW);
    volatile uint64_t* p = (volatile uint64_t*)scratch_va;
    *p = 0xCAFEBABEDEADBEEFull;

    bool ok = f1 != f2 && f1 != 0 && h[0] == 0x1234 && h[15] == 0x5678 &&
              *p == 0xCAFEBABEDEADBEEFull &&
              physical_address(kernel_dir, scratch_va) == scratch_pa;

    serial_print("juampiOS: free frames=");
    serial_dec(free_before);
    serial_print(", heap+paging self-test ");
    serial_print(ok ? "OK\n" : "FAILED\n");
    if (ok) {
        serial_print("juampiOS: memory subsystem OK\n");
    }

    // --- Milestone 2: interrupts (IDT, exceptions, PIC, PIT timer) -----------
    interrupts_init();
    register_interrupt_handler(3, breakpoint_handler); // int3 -> non-fatal
    __asm__ __volatile__("sti");

    // A breakpoint trap must be caught and returned from cleanly...
    __asm__ __volatile__("int3");
    // ...and the timer IRQ must fire and return, advancing the tick count.
    while (timer_ticks() < 3) {
        __asm__ __volatile__("hlt");
    }

    serial_print("juampiOS: int3 handled=");
    serial_dec(bp_hits);
    serial_print(", timer ticks=");
    serial_dec(timer_ticks());
    serial_print("\n");
    if (bp_hits == 1 && timer_ticks() >= 3) {
        serial_print("juampiOS: interrupts OK\n");
    }

    while (1) {
        __asm__ __volatile__("hlt");
    }

#if 0 // PORT64: re-enabled milestone by milestone
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        scrn_setmode(GREEN, BLACK);
        scrn_print("Something went very very wrong. I don't know what else to "
                   "tell you.");
        return;
    }
    scrn_cls();
    scrn_setmode(GREEN, BLACK);
    // Bring up the serial console early so boot progress is visible headlessly
    // (qemu -serial stdio), which is much friendlier for CI than the VGA
    // buffer.
    serial_init();
    serial_print("\n=== juampiOS booting (COM1 serial console) ===\n");
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
    scrn_printf("%u MODULES LOADED\n", mbd->mods_count);

    scrn_print("CHECKING MEMORY STATE\n");
    // We check the amount of RAM memory present.
    if (mbd->flags & 1) {
        scrn_printf("\tAmount of RAM in the system:\n"
                    "\t\tLower: %u Kb, Upper: %u Kb\n",
                    mbd->mem_lower, mbd->mem_upper);
    } else {
        kernel_panic("Invalid GRUB memory map");
    }
    scrn_print("INITIALIZING KERNEL MEMORY STRUCTURES...");
#ifdef KTEST
    // Test builds boot via `qemu -kernel` with no modules, so take the
    // end-of-kernel address from the linker symbol rather than a GRUB module.
    uint kernel_end_addr = (uint)&kernel_end;
#else
    module_t* grub_modules = (module_t*)mbd->mods_addr;
    uint kernel_end_addr = grub_modules[mbd->mods_count - 1].mod_end;
#endif
    // The upper memory map starts from the first megabyte, ergo the first
    // location where we go past the end is 1024 kilobytes plus the memory GRUB
    // reports
    paging_init(kernel_end_addr, (1024 + mbd->mem_upper) * 1024);
    scrn_printf("OK\n");

#ifdef KTEST
    // Memory subsystems are up. Run the integration tests and exit QEMU. We
    // stop before disk/FS/scheduler init, which need hardware and modules the
    // test VM does not provide.
    ktest_main();
    while (1)
        ;
#else
    scrn_print("INITIALIZING ATA DISK\n");
    hdd_init();
    scrn_printf("INITIALIZING EXT2 FILESYSTEM\n");
    init_disk_super_block();
    keybuffer_init(1024);
    scheduler_init();

    serial_print("juampiOS: kernel init complete, entering userland\n");
    void* buffer = (void*)grub_modules[0].mod_start;
    jump_to_initial(buffer);

    while (1)
        ;
#endif
#endif // PORT64
}
