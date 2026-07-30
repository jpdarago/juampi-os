// kernel.c - Startup routine of the operating system kernel.
// Limine jumps to kmain in 64-bit long mode (see docs/x86-64-port.md).

#include <barrier.h>
#include <utils.h>
#include <idt.h>
#include <syscall.h>
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
#include <keyboard.h>
#include <mouse.h>
#include <ktime.h>
#include <rtc.h>
#include <ksym.h>
#include <gfx.h>
#include <ata.h>
#include <nvme.h>
#include <xhci.h>
#include <audio.h>
#include <ext2.h>
#include <smp.h>
#include <parallel.h>
#include <acpi.h>
#include <uacpi_glue.h>
#include <apic.h>
#include <net.h>
#include <tls.h>

#include <printf/printf.h>

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

__attribute__((used, section(".limine_requests"))) static volatile struct
        limine_kernel_file_request kfile_request = {
                .id = LIMINE_KERNEL_FILE_REQUEST, .revision = 0};

__attribute__((
        used,
        section(".limine_requests"))) static volatile struct limine_rsdp_request
        rsdp_request = {.id = LIMINE_RSDP_REQUEST, .revision = 0};

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
static void breakpoint_handler(struct interrupt_frame* f)
{
    (void)f;
    bp_hits++;
}

// Milestone-3 worker threads: each bumps its own counter and yields, so a full
// round-robin proves the context switch preserves every thread independently.
// worker_a also accumulates a double across yields, exercising fxsave/fxrstor:
// its FPU state must survive being switched away and back.
static volatile uint64_t wcounters[3];
static volatile double worker_fp;
static void worker_a(void)
{
    for (;;) {
        wcounters[0]++;
        worker_fp += 0.5;
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

// SMP self-test job: sum the half-open integer range [lo, hi). Each core runs
// one of these on its own slice in parallel; the BSP combines and checks the
// total against the closed form, proving every core executed.
struct sum_job {
    uint64_t lo, hi, result;
};
static void sum_worker(void* p)
{
    struct sum_job* j = p;
    uint64_t s = 0;
    for (uint64_t k = j->lo; k < j->hi; k++) {
        s += k;
    }
    j->result = s;
}

// Stress the segregated heap: many allocations across small size classes and
// large runs, each stamped with a per-block sentinel; free half, allocate more,
// then verify every survivor is intact (catching overlaps/corruption) and that
// freed blocks are reused. Returns true on success.
#define STRESS_N 300
static bool heap_stress(struct allocator* mem, struct heap_allocator* h)
{
    static void* ptrs[STRESS_N];
    static ptrdiff_t sizes[STRESS_N];
    uint64_t rng = 0x9e3779b97f4a7c15ull;

    for (int i = 0; i < STRESS_N; i++) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        // Mostly small, occasionally a large multi-slab run.
        ptrdiff_t sz = ((rng >> 33) % 16 == 0)
                               ? 8192 + (ptrdiff_t)((rng >> 20) % 90000)
                               : 1 + (ptrdiff_t)((rng >> 20) % 4096);
        char* p = alloc(mem, 1, 1, sz);
        for (ptrdiff_t k = 0; k < sz; k++) {
            p[k] = (char)(i + k); // sentinel that depends on block and offset
        }
        ptrs[i] = p;
        sizes[i] = sz;
    }
    // Free every other block.
    for (int i = 0; i < STRESS_N; i += 2) {
        heap_free(h, ptrs[i]);
        ptrs[i] = NULL;
    }
    // Reallocate into the freed space; the survivors must be untouched.
    for (int i = 0; i < STRESS_N; i += 2) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        ptrdiff_t sz = 1 + (ptrdiff_t)((rng >> 20) % 2048);
        ptrs[i] = alloc(mem, 1, 1, sz);
        sizes[i] = sz;
        char* p = ptrs[i];
        for (ptrdiff_t k = 0; k < sz; k++) {
            if (p[k] != 0) {
                return false; // allocations must come back zeroed
            }
            p[k] = (char)(i + k);
        }
    }
    // Verify all live blocks still hold their sentinels (no
    // overlap/corruption).
    for (int i = 0; i < STRESS_N; i++) {
        char* p = ptrs[i];
        for (ptrdiff_t k = 0; k < sizes[i]; k++) {
            if (p[k] != (char)(i + k)) {
                return false;
            }
        }
    }
    return true;
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
        gfx_init(fb_request.response->framebuffers[0]);
    }
    console_print("\n=== juampiOS booting (framebuffer + COM1 console) ===\n");
    console_print("juampiOS: running in 64-bit long mode (booted by Limine)\n");

    // Load the kernel's own symbol table (from Limine) so panics and faults —
    // including any from here on — print symbolized backtraces.
    if (kfile_request.response != NULL) {
        ksym_init(kfile_request.response->kernel_file->address);
    }
    uint64_t sym_off = 0;
    const char* sym_self = ksym_lookup((uint64_t)&kmain, &sym_off);
    console_print("juampiOS: symbols ");
    console_print(sym_self ? "OK (kmain resolved)\n" : "unavailable\n");

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
    console_printf(
            "juampiOS: HHDM offset=0x%lx, memmap entries=%lu, usable RAM=%lu "
            "MiB\n",
            hhdm_request.response->offset, memmap_request.response->entry_count,
            usable / (1024 * 1024));

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
    struct heap_allocator heap = heap_init(
            paging_init(hhdm_request.response->offset, best_base, best_len),
            KHEAP_SIZE);
    struct allocator* mem = &heap.base;
    heap_set_default(&heap); // back the libc shim's malloc/free (Lua)

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

    struct arena scratch = arena_init(new (mem, char, 1024), 1024);
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

    bool stress_ok = heap_stress(mem, &heap);

    bool ok = f1 != f2 && f1 != 0 && heap_zeroed && freelist_reuses &&
              av[3] == 0xA5A5A5A5u && *p == 0xCAFEBABEDEADBEEFull &&
              physical_address(kernel_dir, scratch_va) == scratch_pa &&
              stress_ok;

    console_printf(
            "juampiOS: free frames=%lu, heap stress %s, memory self-test %s\n",
            free_before, stress_ok ? "OK" : "FAILED", ok ? "OK" : "FAILED");
    if (ok) {
        console_print("juampiOS: memory subsystem OK\n");
    }

    // --- Milestone 2: interrupts on the modern APIC (no legacy 8259/PIT) -----
    // Install our own GDT + TSS first (the IDT gates reference its kernel code
    // selector). Then, in dependency order: parse the ACPI tables (the MADT
    // gives the Local/I-O APIC bases, the FADT the PM timer); bring up the IDT
    // + LAPIC + IOAPIC (interrupts_init masks the 8259); calibrate the clock;
    // and start the per-core LAPIC timer that drives the tick.
    gdt_init(mem);

    // Bring up uACPI table access first (barebones mode; see
    // docs/acpi-uacpi.md), then acpi_init() reads the FADT/MADT/DSDT through
    // it.
    uint64_t rsdp =
            rsdp_request.response != NULL
                    ? (uint64_t)(uintptr_t)rsdp_request.response->address
                    : 0;
    if (uacpi_early_tables_init(rsdp)) {
        uacpi_report();
    } else {
        console_print("juampiOS: uacpi early table access unavailable\n");
    }
    acpi_init();

    interrupts_init();      // IDT + LAPIC + IOAPIC; 8259 masked off
    ktime_init();           // TSC via CPUID / ACPI PM timer (PIT-free)
    lapic_timer_start(100); // ~100 Hz periodic tick -> vector 32

    keyboard_init(); // PS/2 keyboard on IRQ 1, routed through the IOAPIC
    mouse_init();    // PS/2 mouse on IRQ 12, routed through the IOAPIC
    register_interrupt_handler(3, breakpoint_handler); // int3 -> non-fatal
    syscall_init(); // int 0x80 syscall boundary for hosted (newlib) programs
    __asm__ __volatile__("sti");

    // A breakpoint trap must be caught and returned from cleanly...
    __asm__ __volatile__("int3");
    // ...and the LAPIC timer IRQ must fire, advancing the tick count. Bound the
    // wait by a TSC budget (spinning, not hlt, so it can time out) so a
    // mis-programmed timer can't hang boot.
    uint64_t irq_guard = rdtsc() + 20000000000ull; // ~4-6 s
    while (timer_ticks() < 3 && rdtsc() < irq_guard) {
        cpu_relax();
    }

    console_printf("juampiOS: int3 handled=%d, timer ticks=%lu\n", bp_hits,
                   timer_ticks());
    if (bp_hits == 1 && timer_ticks() >= 3) {
        console_print("juampiOS: interrupts OK (LAPIC timer + IOAPIC)\n");
    }

    // Full ACPI via uACPI: enter ACPI mode and load the AML namespace, now that
    // the heap, timers and interrupts are up. Enables real _S5 shutdown; falls
    // back to the table-only path (already parsed) if it can't.
    console_printf("juampiOS: uacpi full init %s\n",
                   uacpi_full_init() ? "OK (AML namespace loaded)"
                                     : "failed (tables-only)");

    // --- Timekeeping report (calibrated above). ------------------------------
    uint64_t hz = tsc_hz();
    console_printf(
            "juampiOS: TSC %lu MHz (%s), monotonic clock ns=%lu", hz / 1000000,
            ktime_tsc_invariant() ? "invariant" : "variable", ktime_ns());
    console_print(hz > 100000000ull ? "\njuampiOS: timekeeping OK\n"
                                    : "\njuampiOS: timekeeping FAILED\n");

    // Wall clock (CMOS RTC): monotonic ktime above is since-boot; this is the
    // real date/time, used for TLS cert validity and the k.time/k.date Lua API.
    struct rtc_time now;
    if (rtc_read(&now)) {
        console_printf("juampiOS: rtc %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                       now.year, now.month, now.day, now.hour, now.minute,
                       now.second);
    } else {
        console_print("juampiOS: rtc unavailable\n");
    }

    // --- Block devices + filesystem. Both bring-ups poll with timeouts, so
    // they run after ktime_init(). ext2 then mounts on whichever device carries
    // a valid filesystem, preferring NVMe — the modern path, and the only disk
    // on NVMe-only machines like the target laptop; the ATA data disk (primary
    // IDE slave, separate from the Limine boot master) is the fallback.
    ata_init();
    if (ata_present()) {
        console_printf("juampiOS: ata sectors=%lu\n", ata_sectors());
    } else {
        console_print("juampiOS: ata absent\n");
    }

    nvme_init();
    if (nvme_present()) {
        static uint8_t nvme_blk[4096]; // >= any supported block size
        bool rd = nvme_read(0, 1, nvme_blk);
        console_printf("juampiOS: nvme \"%s\" blocks=%lu blk=%u irq=%s",
                       nvme_model(), nvme_blocks(), nvme_block_size(),
                       nvme_irq_driven() ? "msix" : "polled");
        if (rd) {
            console_print(" read0=OK first8=");
            for (int i = 0; i < 8; i++) {
                console_printf("%02x ", nvme_blk[i]);
            }
            console_printf("(completions=%lu)", nvme_irq_count());
        } else {
            console_print(" read0=FAIL");
        }
        console_print("\n");
    } else if (nvme_fail_reason() != NULL) {
        console_printf("juampiOS: nvme init FAILED (%s)\n", nvme_fail_reason());
    } else {
        console_print("juampiOS: nvme absent\n");
    }

    // --- USB: bring up xHCI, enumerate, and configure a mass-storage stick. --
    xhci_init();
    if (!xhci_present()) {
        if (xhci_fail_reason() != NULL) {
            console_printf("juampiOS: xhci init FAILED (%s)\n",
                           xhci_fail_reason());
        } else {
            console_print("juampiOS: xhci absent\n");
        }
    } else {
        console_printf("juampiOS: xhci up, %u ports irq=%s; %u device(s)",
                       xhci_ports(), xhci_irq_driven() ? "msix" : "polled",
                       xhci_device_count());
        for (uint32_t i = 0; i < xhci_device_count(); i++) {
            uint16_t vid, pid;
            uint8_t cls;
            if (xhci_device_info(i, &vid, &pid, &cls)) {
                console_printf(" %04x:%04x/c%u", vid, pid, cls);
            }
        }
        console_print("\n");
        if (xhci_msc_ready()) {
            console_printf("juampiOS: usb mass storage: %lu blocks x %u bytes "
                           "(events=%lu)\n",
                           xhci_msc_blocks(), xhci_msc_block_size(),
                           xhci_irq_count());
        } else if (xhci_fail_reason() != NULL) {
            console_printf("juampiOS: usb enumeration issue (%s)\n",
                           xhci_fail_reason());
        }
    }

    // --- Audio: bring up the mixer over an AC'97 output, if present.
    // ----------
    audio_init();
    if (audio_present()) {
        console_printf("juampiOS: audio up (%s) irq=%s\n", audio_backend_name(),
                       audio_irq_driven() ? "intx" : "polled");
    } else if (audio_fail_reason() != NULL) {
        console_printf("juampiOS: audio init FAILED (%s)\n",
                       audio_fail_reason());
    } else {
        console_print("juampiOS: audio absent\n");
    }

    // Mount ext2, preferring NVMe, then a USB mass-storage stick, then the ATA
    // disk — so a filesystem on any of them backs the `fs` library.
    const char* fs_where = "not mounted";
    if (nvme_present() && ext2_mount(nvme_blockdev(), &heap)) {
        fs_where = "mounted (nvme)";
    } else if (xhci_msc_ready() && ext2_mount(xhci_msc_blockdev(), &heap)) {
        fs_where = "mounted (usb)";
    } else if (ext2_mount(ata_blockdev(), &heap)) {
        fs_where = "mounted (ata)";
    }
    console_printf("juampiOS: ext2 %s\n", fs_where);

    // --- Networking: bring up the e1000 NIC and a minimal IPv4 stack
    // --- (Ethernet/ARP/IPv4/ICMP), exposed to Lua as `net` (net.ping).
    net_init();

    // --- TLS: sanity-check the vendored BearSSL build (crypto runs
    // freestanding).
    console_print("juampiOS: bearssl ");
    console_print(tls_selftest() ? "OK\n" : "FAILED\n");

    // --- Milestone 8: SMP. Bring up the application processors, then prove ---
    // real parallel execution: partition a big integer sum across all cores
    // (each on its own slice via the work mailbox) and check the combined
    // total.
    smp_init(mem);
    uint64_t ncores = smp_cpu_count();
    const uint64_t SUM_N = 100000000ull; // sum 0..SUM_N-1 across the cores
    struct sum_job* jobs = new (mem, struct sum_job, (ptrdiff_t)ncores);
    uint64_t chunk = SUM_N / ncores;
    for (uint64_t i = 0; i < ncores; i++) {
        jobs[i].lo = i * chunk;
        jobs[i].hi = (i == ncores - 1) ? SUM_N : (i + 1) * chunk;
        jobs[i].result = 0;
    }
    uint32_t bsp = smp_bsp_index();
    for (uint64_t i = 0; i < ncores; i++) {
        if (i != bsp && smp_online((uint32_t)i)) {
            smp_run_on((uint32_t)i, sum_worker, &jobs[i]);
        }
    }
    sum_worker(&jobs[bsp]); // the BSP takes its own slice
    for (uint64_t i = 0; i < ncores; i++) {
        // Only join cores that actually checked in — joining a core that never
        // started would spin forever (matters if an AP fails to come up on real
        // hardware; the BSP already covered its slice).
        if (i != bsp && smp_online((uint32_t)i)) {
            smp_join((uint32_t)i);
        } else if (i != bsp) {
            sum_worker(
                    &jobs[i]); // offline core: the BSP does its slice instead
        }
    }
    uint64_t sum = 0;
    for (uint64_t i = 0; i < ncores; i++) {
        sum += jobs[i].result;
    }
    bool smp_ok = sum == SUM_N * (SUM_N - 1) / 2;
    console_printf("juampiOS: SMP %s: %lu cores (parallel sum %s)\n",
                   smp_ok ? "OK" : "FAILED", ncores,
                   smp_ok ? "verified" : "MISMATCH");

    // --- Milestone 9: parallel Lua. A lua_State per core (each with its own
    // --- heap, so allocation is lock-free), driven from Lua via the thread/mem
    // libraries. The self-test runs a tiny Lua chunk on every core.
    parallel_init(mem);
    console_print(parallel_selftest() ? "juampiOS: parallel Lua OK\n"
                                      : "juampiOS: parallel Lua FAILED\n");

    // --- Milestone 3: software context switch (kernel threads) --------------
    sched_init(mem);
    thread_create(mem, worker_a);
    thread_create(mem, worker_b);
    thread_create(mem, worker_c);
    // Cooperatively round-robin: each yield hands off to the next thread and
    // eventually returns here, proving the switch preserves and restores each
    // thread's stack and registers independently.
    while (wcounters[0] < 5 || wcounters[1] < 5 || wcounters[2] < 5) {
        yield();
    }

    console_printf("juampiOS: thread ticks a=%lu b=%lu c=%lu\n"
                   "juampiOS: context switch OK\n",
                   wcounters[0], wcounters[1], wcounters[2]);

    // --- Floating point: SSE enabled at entry, FP state saved across switches.
    // Kernel double arithmetic (SSE) and FP preserved through worker_a's yields
    // (worker_fp += 0.5 five times ~= 2.5).
    volatile double one = 1.0, three = 3.0;
    double back = (one / three) * three; // ~1.0 via divsd/mulsd
    bool fp_ok = back > 0.999 && back < 1.001 && worker_fp > 2.49 &&
                 worker_fp < 2.51;
    console_printf("juampiOS: fp roundtrip (1/3*3=%lu/1000), worker_fp*10=%lu",
                   (uint64_t)(back * 1000.0),     // 1000
                   (uint64_t)(worker_fp * 10.0)); // 25
    console_print(fp_ok ? "\njuampiOS: floating point OK\n"
                        : "\njuampiOS: floating point FAILED\n");

    // Vendored float-capable printf (the number formatting Lua will need).
    char pbuf[64];
    snprintf(pbuf, sizeof(pbuf), "%.4f pi, %d, %#x, %g", 3.14159265, -42,
             0xBEEF, 1.5e-3);
    console_print("juampiOS: printf test: ");
    console_print(pbuf);
    console_print("\n");

    // Boot self-tests done; hand control to the interactive shell. (The ring-3
    // ELF64 path from the port stays available in elf64.c / gdt64.c for when
    // isolation is wanted, but the kernel shell runs in ring 0 for full
    // access.)
    console_print("juampiOS: boot complete\n");
    shell_run(&heap);
}
