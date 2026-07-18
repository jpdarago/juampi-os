// kernel.c - Startup routine of the operating system kernel.
// Limine jumps to kmain in 64-bit long mode (see docs/x86-64-port.md).

#include <utils.h>
#include <idt.h>
#include <memory.h>
#include <frames.h>
#include <paging.h>
#include <serial.h>
#include <limine.h>
#include <sched.h>
#include <gdt64.h>
#include <elf64.h>

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

__attribute__((used, section(".limine_requests"))) static volatile struct
        limine_module_request module_request = {.id = LIMINE_MODULE_REQUEST,
                                                .revision = 0};

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

// Milestone-3 worker threads: each bumps its own counter and yields, so a full
// round-robin proves the context switch preserves every thread independently.
static volatile uint64 wcounters[3];
static void worker_a(void)
{
    for (;;) {
        wcounters[0]++;
        yield();
    }
}
static void worker_b(void)
{
    for (;;) {
        wcounters[1]++;
        yield();
    }
}
static void worker_c(void)
{
    for (;;) {
        wcounters[2]++;
        yield();
    }
}

// int 0x80 syscall handler. Port ABI: number in rax, args in rdi/rsi, return in
// rax. The user pointer in write() is validated with the boundary guard before
// the kernel touches it.
static void syscall_handler(interrupt_frame* f)
{
    switch (f->rax) {
    case 1: { // write(buf = rdi, len = rsi)
        const char* buf = (const char*)f->rdi;
        uint64 len = f->rsi;
        if (!user_access_ok((uintptr)buf, len, false)) {
            f->rax = (uint64)-1;
            break;
        }
        for (uint64 i = 0; i < len; i++) {
            serial_putc(buf[i]);
        }
        f->rax = len;
        break;
    }
    case 2: // exit(code = rdi): the user program is done
        serial_print("juampiOS: userland OK\n");
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    default:
        f->rax = (uint64)-1;
    }
}

// kmain is the ELF entry point; Limine jumps here in 64-bit long mode. Each
// subsystem is brought up in dependency order and proves itself with a serial
// self-test; the boot-smoke test greps for the final marker.
void kmain(void)
{
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
    // Install our own GDT + TSS first (kernel + user segments); the IDT gates
    // then reference its kernel code selector, and the TSS supplies the ring-0
    // stack used on the ring-3 -> ring-0 transition in milestone 4.
    gdt_init();
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

    // --- Milestone 3: software context switch (kernel threads) --------------
    sched_init();
    thread_create(worker_a);
    thread_create(worker_b);
    thread_create(worker_c);
    // Cooperatively round-robin: each yield hands off to the next thread and
    // eventually returns here, proving the switch preserves and restores each
    // thread's stack and registers independently.
    while (wcounters[0] < 5 || wcounters[1] < 5 || wcounters[2] < 5) {
        yield();
    }

    serial_print("juampiOS: thread ticks a=");
    serial_dec(wcounters[0]);
    serial_print(" b=");
    serial_dec(wcounters[1]);
    serial_print(" c=");
    serial_dec(wcounters[2]);
    serial_print("\njuampiOS: context switch OK\n");

    // --- Milestone 5: load a real ELF64 user program (Limine module) and run
    // it in ring 3, servicing its int 0x80 syscalls. This exercises the whole
    // stack built across milestones 0-4.
    register_interrupt_handler(0x80, syscall_handler);
    if (module_request.response == NULL ||
        module_request.response->module_count < 1) {
        early_halt("juampiOS: PANIC - no user module provided\n");
    }
    struct limine_file* mod = module_request.response->modules[0];
    uint64 entry = elf64_load(mod->address);
    if (entry == 0) {
        early_halt("juampiOS: PANIC - user module is not a valid ELF64\n");
    }

    // Give the program a user stack, then drop to ring 3 at its entry point.
    uintptr ustack_va = 0x7000000;
    map_page(kernel_dir, ustack_va, frame_alloc(),
             PAGEF_P | PAGEF_RW | PAGEF_U);

    serial_print("juampiOS: entering ELF64 userland...\n");
    enter_user_mode(entry, ustack_va + PAGE_SZ);

    while (1) {
        __asm__ __volatile__("hlt");
    }
}
