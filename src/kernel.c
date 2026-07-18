// kernel.c - Startup routine of the operating system kernel.
// Limine jumps to kmain in 64-bit long mode (see docs/x86-64-port.md).

#include <utils.h>
#include <idt.h>
#include <alloc.h>
#include <arena.h>
#include <memory.h>
#include <frames.h>
#include <paging.h>
#include <serial.h>
#include <limine.h>
#include <sched.h>
#include <gdt64.h>
#include <shell.h>
#include <console.h>

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
        limine_framebuffer_request fb_request = {
                .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0};

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
    console_print(msg);
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
static volatile uint64_t wcounters[3];
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

// kmain is the ELF entry point; Limine jumps here in 64-bit long mode. Each
// subsystem is brought up in dependency order and proves itself with a serial
// self-test; the boot-smoke test greps for the final marker.
void kmain(void)
{
    serial_init();

    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        early_halt("juampiOS: PANIC - Limine base revision unsupported\n");
    }
    if (memmap_request.response == NULL || hhdm_request.response == NULL) {
        early_halt("juampiOS: PANIC - Limine did not answer boot requests\n");
    }

    // Bring up the framebuffer terminal as early as possible so the whole boot
    // log is visible on screen; output is mirrored to serial throughout.
    if (fb_request.response != NULL &&
        fb_request.response->framebuffer_count > 0) {
        console_init(fb_request.response->framebuffers[0]);
    }
    console_print("\n=== juampiOS booting (framebuffer + COM1 console) ===\n");
    console_print("juampiOS: running in 64-bit long mode (booted by Limine)\n");

    // Prove the protocol works end to end: report the higher-half offset and
    // the usable-RAM total the bootloader gave us.
    console_print("juampiOS: Limine boot protocol OK\n");
    uint64_t usable = 0;
    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry* e = memmap_request.response->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            usable += e->length;
        }
    }
    console_print("juampiOS: HHDM offset=");
    console_hex(hhdm_request.response->offset);
    console_print(", memmap entries=");
    console_dec(memmap_request.response->entry_count);
    console_print(", usable RAM=");
    console_dec(usable / (1024 * 1024));
    console_print(" MiB\n");

    // --- Milestone 1: frame allocator + 4-level paging + kernel heap --------
    // Use the largest usable region Limine reported as the physical frame pool.
    uintptr_t best_base = 0, best_len = 0;
    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry* e = memmap_request.response->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length > best_len) {
            best_base = e->base;
            best_len = e->length;
        }
    }
    // The kernel heap (alloc.h interface) lives in the window paging_init
    // mapped; long-lived subsystem allocations draw from it via `allocator*`.
    heap_allocator heap = heap_init(
            paging_init(hhdm_request.response->offset, best_base, best_len),
            KHEAP_SIZE);
    allocator* mem = &heap.base;

    // Self-test: distinct frames, writable zeroed heap and arena blocks (the
    // arena carved out of the heap), heap_free round-trip, and a fresh 4-level
    // mapping that round-trips a value and resolves back to its frame.
    uintptr_t free_before = frames_available();
    uintptr_t f1 = frame_alloc();
    uintptr_t f2 = frame_alloc();
    int* h = new (mem, int, 16);
    bool heap_zeroed = h[0] == 0 && h[15] == 0;
    h[0] = 0x1234;
    h[15] = 0x5678;

    arena scratch = arena_init(new (mem, char, 1024), 1024);
    uint64_t* av = new (&scratch.base, uint64_t, 4);
    av[3] = 0xA5A5A5A5u;
    void* before_free = h;
    heap_free(&heap, h);
    int* h2 = new (mem, int, 16); // should reuse the freed block
    bool freelist_reuses = h2 == before_free && h2[0] == 0;

    uintptr_t scratch_va = 0xffffd00000000000ull;
    uintptr_t scratch_pa = frame_alloc();
    map_page(kernel_dir, scratch_va, scratch_pa, PAGEF_P | PAGEF_RW);
    volatile uint64_t* p = (volatile uint64_t*)scratch_va;
    *p = 0xCAFEBABEDEADBEEFull;

    bool ok = f1 != f2 && f1 != 0 && heap_zeroed && freelist_reuses &&
              av[3] == 0xA5A5A5A5u && *p == 0xCAFEBABEDEADBEEFull &&
              physical_address(kernel_dir, scratch_va) == scratch_pa;

    console_print("juampiOS: free frames=");
    console_dec(free_before);
    console_print(", heap+paging self-test ");
    console_print(ok ? "OK\n" : "FAILED\n");
    if (ok) {
        console_print("juampiOS: memory subsystem OK\n");
    }

    // --- Milestone 2: interrupts (IDT, exceptions, PIC, PIT timer) -----------
    // Install our own GDT + TSS first (kernel + user segments); the IDT gates
    // then reference its kernel code selector, and the TSS supplies the ring-0
    // stack used on the ring-3 -> ring-0 transition in milestone 4.
    gdt_init(mem);
    interrupts_init();
    register_interrupt_handler(3, breakpoint_handler); // int3 -> non-fatal
    __asm__ __volatile__("sti");

    // A breakpoint trap must be caught and returned from cleanly...
    __asm__ __volatile__("int3");
    // ...and the timer IRQ must fire and return, advancing the tick count.
    while (timer_ticks() < 3) {
        __asm__ __volatile__("hlt");
    }

    console_print("juampiOS: int3 handled=");
    console_dec(bp_hits);
    console_print(", timer ticks=");
    console_dec(timer_ticks());
    console_print("\n");
    if (bp_hits == 1 && timer_ticks() >= 3) {
        console_print("juampiOS: interrupts OK\n");
    }

    // --- Milestone 3: software context switch (kernel threads) --------------
    sched_init();
    thread_create(mem, worker_a);
    thread_create(mem, worker_b);
    thread_create(mem, worker_c);
    // Cooperatively round-robin: each yield hands off to the next thread and
    // eventually returns here, proving the switch preserves and restores each
    // thread's stack and registers independently.
    while (wcounters[0] < 5 || wcounters[1] < 5 || wcounters[2] < 5) {
        yield();
    }

    console_print("juampiOS: thread ticks a=");
    console_dec(wcounters[0]);
    console_print(" b=");
    console_dec(wcounters[1]);
    console_print(" c=");
    console_dec(wcounters[2]);
    console_print("\njuampiOS: context switch OK\n");

    // Boot self-tests done; hand control to the interactive shell. (The ring-3
    // ELF64 path from the port stays available in elf64.c / gdt64.c for when
    // isolation is wanted, but the kernel shell runs in ring 0 for full
    // access.)
    console_print("juampiOS: boot complete\n");
    shell_run();
}
