// uACPI integration shim (see uacpi_glue.h). Provides the kernel callbacks the
// vendored uACPI needs — everything from table access up to the full AML
// interpreter — plus early table-access setup, full init, and AML-based
// shutdown. Every physical address uACPI asks us to map is already reachable
// through the Limine HHDM, so map/unmap are trivial.
//
// This is a single-core, BSP-only, cooperatively-scheduled kernel and uACPI is
// only ever driven from the boot thread, so the concurrency primitives
// collapse: mutexes/events are trivial, spinlocks are interrupt-disable,
// "threads" are one, and scheduled work runs synchronously. That is sufficient
// for init + S5; a real work queue would be needed for heavy runtime GPE
// handling.

#include <uacpi_glue.h>
#include <acpi.h> // acpi_shutdown for the power-button handler
#include <uacpi/kernel_api.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>
#include <uacpi/uacpi.h>
#include <uacpi/sleep.h>
#include <uacpi/event.h>

#include <paging.h>  // phys_to_virt, hhdm_offset
#include <memory.h>  // heap for uacpi_kernel_alloc/free
#include <ports.h>   // in/out for io + pci helpers
#include <pci.h>     // pci_read32/write32
#include <ktime.h>   // nanoseconds, stall/sleep
#include <idt.h>     // register_interrupt_handler, irq_unmask
#include <barrier.h> // cpu_relax
#include <console.h>

static uacpi_phys_addr g_rsdp; // physical address of the RSDP
static bool g_full_init;       // full uacpi_initialize + namespace succeeded

// --- kernel callbacks required by barebones uACPI ---------------------------

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out_rsdp_address)
{
    if (g_rsdp == 0) {
        return UACPI_STATUS_NOT_FOUND;
    }
    *out_rsdp_address = g_rsdp;
    return UACPI_STATUS_OK;
}

// All physical memory is identity-mapped through the HHDM, so a "mapping" is
// just the offset view; there is nothing to tear down.
void* uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
    (void)len;
    return phys_to_virt((uintptr_t)addr);
}
void uacpi_kernel_unmap(void* addr, uacpi_size len)
{
    (void)addr;
    (void)len;
}

// uACPI pre-formats the message (non-formatted logging) and appends a newline,
// so we just emit it. Prefix so it's clear in the boot log.
void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* msg)
{
    (void)level;
    console_print("uacpi: ");
    console_print(msg);
}

// --- memory -----------------------------------------------------------------
void* uacpi_kernel_alloc(uacpi_size size)
{
    return alloc(&heap_default()->base, 1, 1, (ptrdiff_t)size);
}
void uacpi_kernel_free(void* mem)
{
    if (mem != NULL) {
        heap_free(heap_default(), mem);
    }
}

// --- time -------------------------------------------------------------------
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
    return ktime_ns();
}
void uacpi_kernel_stall(uacpi_u8 usec)
{
    uint64_t end = ktime_ns() + (uint64_t)usec * 1000;
    while (ktime_ns() < end) {
        cpu_relax();
    }
}
void uacpi_kernel_sleep(uacpi_u64 msec)
{
    // Cooperative kernel: busy-wait (uACPI only sleeps during rare AML delays).
    uint64_t end = ktime_ms() + msec;
    while (ktime_ms() < end) {
        cpu_relax();
    }
}
uacpi_thread_id uacpi_kernel_get_thread_id(void)
{
    return (uacpi_thread_id)1; // single boot thread; must not be _NONE
}

// --- interrupt state + spinlocks (single core: disable interrupts) ----------
static uacpi_cpu_flags irq_save_cli(void)
{
    uacpi_cpu_flags f;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(f)::"memory");
    return f;
}
static void irq_restore(uacpi_cpu_flags f)
{
    __asm__ __volatile__("push %0; popfq" ::"r"(f) : "memory", "cc");
}
uacpi_interrupt_state uacpi_kernel_disable_interrupts(void)
{
    return irq_save_cli();
}
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state)
{
    irq_restore(state);
}
uacpi_handle uacpi_kernel_create_spinlock(void)
{
    return alloc(&heap_default()->base, 1, 1, 1); // distinct non-null handle
}
void uacpi_kernel_free_spinlock(uacpi_handle h)
{
    if (h != NULL) {
        heap_free(heap_default(), h);
    }
}
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h)
{
    (void)h;
    return irq_save_cli();
}
void uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags f)
{
    (void)h;
    irq_restore(f);
}

// --- mutexes: single-threaded, so trivially uncontended ---------------------
uacpi_handle uacpi_kernel_create_mutex(void)
{
    return alloc(&heap_default()->base, 1, 1, 1);
}
void uacpi_kernel_free_mutex(uacpi_handle h)
{
    if (h != NULL) {
        heap_free(heap_default(), h);
    }
}
uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle h, uacpi_u16 timeout)
{
    (void)h;
    (void)timeout;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_release_mutex(uacpi_handle h)
{
    (void)h;
}

// --- events: a signal counter (nothing async signals during a wait here) ----
uacpi_handle uacpi_kernel_create_event(void)
{
    return alloc(&heap_default()->base, 1, 1, sizeof(int)); // zeroed count
}
void uacpi_kernel_free_event(uacpi_handle h)
{
    if (h != NULL) {
        heap_free(heap_default(), h);
    }
}
void uacpi_kernel_signal_event(uacpi_handle h)
{
    (*(volatile int*)h)++;
}
void uacpi_kernel_reset_event(uacpi_handle h)
{
    *(volatile int*)h = 0;
}
uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle h, uacpi_u16 timeout)
{
    volatile int* count = h;
    if (*count > 0) {
        (*count)--;
        return UACPI_TRUE;
    }
    if (timeout == 0xFFFF) {
        return UACPI_FALSE; // nothing will ever signal on this thread
    }
    uint64_t end = ktime_ms() + timeout;
    while (ktime_ms() < end) {
        if (*count > 0) {
            (*count)--;
            return UACPI_TRUE;
        }
        cpu_relax();
    }
    return UACPI_FALSE;
}

// --- I/O ports (handle is just the base port) -------------------------------
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len,
                                 uacpi_handle* out)
{
    (void)len;
    *out = (uacpi_handle)(uintptr_t)base;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle h)
{
    (void)h;
}
uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size off, uacpi_u8* v)
{
    *v = inb((uint16_t)((uintptr_t)h + off));
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size off,
                                    uacpi_u16* v)
{
    *v = inw((uint16_t)((uintptr_t)h + off));
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size off,
                                    uacpi_u32* v)
{
    *v = inl((uint16_t)((uintptr_t)h + off));
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v)
{
    outb((uint16_t)((uintptr_t)h + off), v);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size off,
                                     uacpi_u16 v)
{
    outw((uint16_t)((uintptr_t)h + off), v);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size off,
                                     uacpi_u32 v)
{
    outl((uint16_t)((uintptr_t)h + off), v);
    return UACPI_STATUS_OK;
}

// --- PCI config space (legacy 0xCF8/0xCFC via pci_read32/write32) -----------
typedef struct {
    uint8_t bus, dev, func;
} pci_dev;

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address,
                                          uacpi_handle* out)
{
    if (address.segment != 0) {
        return UACPI_STATUS_NOT_FOUND; // legacy config: segment 0 only
    }
    pci_dev* d = alloc(&heap_default()->base, 1, 1, sizeof *d);
    d->bus = address.bus;
    d->dev = address.device;
    d->func = address.function;
    *out = d;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle h)
{
    if (h != NULL) {
        heap_free(heap_default(), h);
    }
}
static uint32_t pci_cfg_dword(pci_dev* d, uacpi_size off)
{
    return pci_read32(d->bus, d->dev, d->func, (uint8_t)(off & ~3u));
}
uacpi_status uacpi_kernel_pci_read8(uacpi_handle h, uacpi_size off, uacpi_u8* v)
{
    *v = (uint8_t)(pci_cfg_dword(h, off) >> ((off & 3) * 8));
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_read16(uacpi_handle h, uacpi_size off,
                                     uacpi_u16* v)
{
    *v = (uint16_t)(pci_cfg_dword(h, off) >> ((off & 2) * 8));
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_read32(uacpi_handle h, uacpi_size off,
                                     uacpi_u32* v)
{
    *v = pci_cfg_dword(h, off);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v)
{
    pci_dev* d = h;
    uint32_t dw = pci_cfg_dword(d, off);
    uint32_t sh = (off & 3) * 8;
    dw = (dw & ~(0xFFu << sh)) | ((uint32_t)v << sh);
    pci_write32(d->bus, d->dev, d->func, (uint8_t)(off & ~3u), dw);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write16(uacpi_handle h, uacpi_size off,
                                      uacpi_u16 v)
{
    pci_dev* d = h;
    uint32_t dw = pci_cfg_dword(d, off);
    uint32_t sh = (off & 2) * 8;
    dw = (dw & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    pci_write32(d->bus, d->dev, d->func, (uint8_t)(off & ~3u), dw);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write32(uacpi_handle h, uacpi_size off,
                                      uacpi_u32 v)
{
    pci_dev* d = h;
    pci_write32(d->bus, d->dev, d->func, (uint8_t)(off & ~3u), v);
    return UACPI_STATUS_OK;
}

// --- interrupts: the ACPI SCI (a single line) -------------------------------
static uacpi_interrupt_handler g_sci_handler;
static uacpi_handle g_sci_ctx;
static void sci_trampoline(interrupt_frame* f)
{
    (void)f;
    if (g_sci_handler != NULL) {
        g_sci_handler(g_sci_ctx);
    }
}
uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq,
                                                    uacpi_interrupt_handler h,
                                                    uacpi_handle ctx,
                                                    uacpi_handle* out_handle)
{
    g_sci_handler = h;
    g_sci_ctx = ctx;
    register_interrupt_handler(32 + irq, sci_trampoline);
    irq_unmask(irq);
    *out_handle = (uacpi_handle)(uintptr_t)(irq + 1);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler h,
                                                      uacpi_handle handle)
{
    (void)h;
    (void)handle;
    g_sci_handler = NULL;
    return UACPI_STATUS_OK;
}

// --- deferred work: run synchronously (single-core cooperative) -------------
uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type,
                                        uacpi_work_handler h, uacpi_handle ctx)
{
    (void)type;
    h(ctx);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
    return UACPI_STATUS_OK; // work runs synchronously, so nothing is pending
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* req)
{
    (void)req; // breakpoint / fatal AML ops — nothing to do
    return UACPI_STATUS_OK;
}

// --- setup + report ---------------------------------------------------------

// A scratch buffer uACPI uses to track table descriptors before the heap is
// handed over (barebones early access does no dynamic allocation).
static uint8_t early_table_buf[4096];

bool uacpi_early_tables_init(uint64_t rsdp_addr)
{
    if (rsdp_addr == 0) {
        return false;
    }
    // Limine may hand us the RSDP as an HHDM virtual address; uACPI wants the
    // physical one.
    g_rsdp = rsdp_addr >= hhdm_offset ? rsdp_addr - hhdm_offset : rsdp_addr;

    uacpi_status st = uacpi_setup_early_table_access(early_table_buf,
                                                     sizeof early_table_buf);
    return st == UACPI_STATUS_OK;
}

void uacpi_report(void)
{
    struct acpi_fadt* fadt = NULL;
    uacpi_table madt;
    bool have_fadt = uacpi_table_fadt(&fadt) == UACPI_STATUS_OK && fadt != NULL;
    bool have_madt = uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE,
                                                   &madt) == UACPI_STATUS_OK;
    console_printf("juampiOS: uacpi v%d.%d.%d tables: FADT=%s MADT=%s\n",
                   UACPI_MAJOR, UACPI_MINOR, UACPI_PATCH,
                   have_fadt ? "ok" : "-", have_madt ? "ok" : "-");
}

// Fixed power-button event (QMP `system_powerdown` / a physical press): power
// off gracefully. Runs from the SCI, but acpi_shutdown() never returns, so
// re-entrancy doesn't matter.
static uacpi_interrupt_ret power_button_handler(uacpi_handle ctx)
{
    (void)ctx;
    console_print("juampiOS: power button -> shutdown\n");
    acpi_shutdown();
    return UACPI_INTERRUPT_HANDLED; // unreachable
}

// Full bring-up: enter ACPI mode, load and initialize the namespace so the AML
// interpreter is usable, then enable GPEs and the power button. Must run after
// the heap, timers and interrupts are up.
bool uacpi_full_init(void)
{
    if (uacpi_initialize(0) != UACPI_STATUS_OK) {
        return false;
    }
    if (uacpi_namespace_load() != UACPI_STATUS_OK) {
        return false;
    }
    if (uacpi_namespace_initialize() != UACPI_STATUS_OK) {
        return false;
    }
    g_full_init = true;

    // Best-effort: enable GPE handling and a graceful power-button shutdown.
    uacpi_finalize_gpe_initialization();
    uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON,
                                      power_button_handler, UACPI_NULL);
    return true;
}

bool uacpi_full_init_done(void)
{
    return g_full_init;
}

// Power off via AML-evaluated _S5, if the namespace is up. Returns on failure
// (the caller falls back to the legacy PM1 path); does not return on success.
void uacpi_try_shutdown(void)
{
    if (!g_full_init) {
        return;
    }
    if (uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5) !=
        UACPI_STATUS_OK) {
        return;
    }
    uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5); // no return on success
}
