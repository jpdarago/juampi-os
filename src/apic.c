// The APIC interrupt hardware (see apic.h): a Local APIC per core plus the I/O
// APIC, replacing the legacy 8259 PIC/PIT. The LAPIC is driven through x2APIC
// (MSRs) when the CPU supports it, else xAPIC (an uncached MMIO page); the two
// are hidden behind lapic_read/write. Bases come from the ACPI MADT, never
// hardcoded (though the architectural defaults are used as a backstop).

#include <apic.h>
#include <acpi.h>
#include <ktime.h>
#include <paging.h>

#include <stdint.h>
#include <stdbool.h>

#define IA32_APIC_BASE 0x1B
#define APIC_BASE_ENABLE (1u << 11) // global enable
#define APIC_BASE_EXTD (1u << 10)   // x2APIC mode

// LAPIC register byte offsets (xAPIC). The x2APIC MSR index is 0x800 + off/16.
#define REG_ID 0x020
#define REG_EOI 0x0B0
#define REG_SVR 0x0F0 // spurious interrupt vector register
#define REG_LVT_TIMER 0x320
#define REG_TIMER_INIT 0x380
#define REG_TIMER_CUR 0x390
#define REG_TIMER_DIV 0x3E0

#define SVR_ENABLE (1u << 8)    // software-enable the LAPIC
#define LVT_MASKED (1u << 16)   // mask an LVT entry
#define LVT_PERIODIC (1u << 17) // timer periodic mode

#define SPURIOUS_VECTOR 0x2F
#define TIMER_VECTOR 32

// Dedicated uncached VA windows for xAPIC / IOAPIC MMIO (clear of the gfx
// framebuffer window at ...0000000 and the e1000 window at ...2000000).
#define LAPIC_VA 0xffffe00003000000ull
#define IOAPIC_VA 0xffffe00003001000ull

static bool x2mode;
static volatile uint32_t* lapic; // xAPIC MMIO base (NULL in x2APIC mode)

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ __volatile__("wrmsr" ::"c"(msr), "a"((uint32_t)v),
                         "d"((uint32_t)(v >> 32)));
}

static uint32_t lapic_read(uint32_t off)
{
    return x2mode ? (uint32_t)rdmsr(0x800 + (off >> 4)) : lapic[off / 4];
}
static void lapic_write(uint32_t off, uint32_t val)
{
    if (x2mode) {
        wrmsr(0x800 + (off >> 4), val);
    } else {
        lapic[off / 4] = val;
    }
}

void lapic_eoi(void)
{
    lapic_write(REG_EOI, 0);
}

uint32_t lapic_id(void)
{
    uint32_t v = lapic_read(REG_ID);
    return x2mode ? v : (v >> 24); // xAPIC id lives in bits 31:24
}

static bool cpu_has_x2apic(void)
{
    uint32_t a, b, c, d;
    __asm__ __volatile__("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(1u), "c"(0u));
    return (c & (1u << 21)) != 0; // CPUID.1:ECX.x2APIC
}

void apic_init(void)
{
    x2mode = cpu_has_x2apic();

    if (!x2mode) {
        uint64_t phys = acpi_lapic_base();
        if (phys == 0) {
            phys = 0xFEE00000ull; // architectural default
        }
        map_page(kernel_dir, LAPIC_VA, (uintptr_t)phys,
                 PAGEF_P | PAGEF_RW | PAGEF_UC);
        lapic = (volatile uint32_t*)LAPIC_VA;
    }

    // Enable the LAPIC in the base MSR (and enter x2APIC mode if supported).
    uint64_t base = rdmsr(IA32_APIC_BASE) | APIC_BASE_ENABLE;
    if (x2mode) {
        base |= APIC_BASE_EXTD;
    }
    wrmsr(IA32_APIC_BASE, base);

    // Software-enable + set the spurious-interrupt vector.
    lapic_write(REG_SVR, SVR_ENABLE | SPURIOUS_VECTOR);
}

void lapic_timer_start(uint32_t hz)
{
    lapic_write(REG_TIMER_DIV, 0x3); // divide the bus clock by 16

    // Calibrate: count down from max, masked (no interrupt), over ~10 ms of the
    // monotonic clock (which is TSC/PM-timer based — no PIT), and see how far
    // it got. This yields the timer's tick rate without any legacy timer.
    lapic_write(REG_LVT_TIMER, LVT_MASKED);
    lapic_write(REG_TIMER_INIT, 0xFFFFFFFFu);
    uint64_t t0 = ktime_ns();
    while (ktime_ns() - t0 < 10000000ull) {
        __asm__ __volatile__("pause");
    }
    uint32_t counted = 0xFFFFFFFFu - lapic_read(REG_TIMER_CUR);
    lapic_write(REG_TIMER_INIT, 0); // stop

    uint64_t per_sec = (uint64_t)counted * 100ull; // 10 ms -> * 100
    uint32_t initial = (uint32_t)(per_sec / (hz ? hz : 100));
    if (initial == 0) {
        initial = 1;
    }
    // Periodic, vector 32, unmasked.
    lapic_write(REG_LVT_TIMER, LVT_PERIODIC | TIMER_VECTOR);
    lapic_write(REG_TIMER_INIT, initial);
}

// --- I/O APIC ---------------------------------------------------------------
// Indirect register access: write the register index to IOREGSEL (base+0), then
// read/write the 32-bit value at IOWIN (base+0x10). A redirection entry for
// global system interrupt g is the 64-bit pair of registers 0x10+2g / 0x11+2g.

static volatile uint32_t* ioapic; // IOAPIC MMIO (NULL if none)
static uint32_t ioapic_gsi_base;

static uint32_t ioapic_read(uint32_t reg)
{
    ioapic[0] = reg;  // IOREGSEL
    return ioapic[4]; // IOWIN (0x10 / 4)
}
static void ioapic_write(uint32_t reg, uint32_t val)
{
    ioapic[0] = reg;
    ioapic[4] = val;
}

void ioapic_init(void)
{
    uint64_t phys = 0;
    uint32_t gsi_base = 0;
    if (!acpi_ioapic(&phys, &gsi_base)) {
        return;
    }
    map_page(kernel_dir, IOAPIC_VA, (uintptr_t)phys,
             PAGEF_P | PAGEF_RW | PAGEF_UC);
    ioapic = (volatile uint32_t*)IOAPIC_VA;
    ioapic_gsi_base = gsi_base;

    // Mask every redirection entry to start from a known, quiet state.
    uint32_t maxredir = (ioapic_read(0x01) >> 16) & 0xFF;
    for (uint32_t i = 0; i <= maxredir; i++) {
        ioapic_write(0x10 + 2 * i, LVT_MASKED);
        ioapic_write(0x11 + 2 * i, 0);
    }
}

void ioapic_route(uint32_t gsi, uint8_t vector, uint32_t dest_apic_id,
                  uint16_t flags)
{
    if (ioapic == NULL) {
        return;
    }
    uint32_t idx = gsi - ioapic_gsi_base;
    uint32_t lo = vector; // fixed delivery, physical dest, unmasked
    if ((flags & 0x3) == 0x3) {
        lo |= (1u << 13); // active low
    }
    if (((flags >> 2) & 0x3) == 0x3) {
        lo |= (1u << 15); // level triggered
    }
    ioapic_write(0x11 + 2 * idx, dest_apic_id << 24);
    ioapic_write(0x10 + 2 * idx, lo);
}

void ioapic_mask(uint32_t gsi)
{
    if (ioapic == NULL) {
        return;
    }
    uint32_t idx = gsi - ioapic_gsi_base;
    ioapic_write(0x10 + 2 * idx, ioapic_read(0x10 + 2 * idx) | LVT_MASKED);
}
