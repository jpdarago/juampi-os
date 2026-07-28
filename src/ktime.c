#include <ktime.h>
#include <acpi.h> // ACPI PM timer (PIT-free calibration fallback)
#include <ports.h>

#define PM_TIMER_HZ 3579545u // the ACPI PM timer's fixed frequency

// CPUID leaves used for PIT-free TSC calibration.
#define CPUID_EXT_MAX 0x80000000u   // highest extended leaf supported
#define CPUID_EXT_POWER 0x80000007u // power mgmt: EDX bit 8 = invariant TSC
#define CPUID_LEAF_TSC_FREQ 0x15u   // TSC/crystal ratio + crystal Hz
#define CPUID_LEAF_CPU_FREQ 0x16u   // processor base frequency (MHz)

static uint64_t g_tsc_hz;
static uint64_t g_tsc_base;
static bool g_invariant;

static bool detect_invariant_tsc(void)
{
    uint32_t eax, ebx, ecx, edx;
    // EDX bit 8 of the power-mgmt leaf = invariant TSC. Guard on the max
    // extended leaf first.
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(CPUID_EXT_MAX));
    if (eax < CPUID_EXT_POWER) {
        return false;
    }
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(CPUID_EXT_POWER));
    return (edx & (1u << 8)) != 0;
}

// The TSC frequency straight from CPUID leaf 0x15 (crystal * ratio), present on
// Skylake and later — so timekeeping does not depend on the PIT ticking, which
// matters on real hardware where the legacy PIT may be dead. Returns 0 when the
// leaf is unavailable or does not enumerate the crystal.
static uint64_t tsc_hz_from_cpuid(void)
{
    uint32_t a, b, c, d;
    __asm__ __volatile__("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0u));
    uint32_t max_leaf = a;
    if (max_leaf < CPUID_LEAF_TSC_FREQ) {
        return 0;
    }
    // eax = ratio denominator, ebx = numerator, ecx = crystal Hz.
    __asm__ __volatile__("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(CPUID_LEAF_TSC_FREQ), "c"(0u));
    if (a != 0 && b != 0 && c != 0) {
        return (uint64_t)c * b / a;
    }
    // Crystal not enumerated: fall back to the CPU-frequency leaf (eax = base
    // MHz), which for an invariant TSC is the TSC rate.
    if (max_leaf >= CPUID_LEAF_CPU_FREQ) {
        __asm__ __volatile__("cpuid"
                             : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(CPUID_LEAF_CPU_FREQ), "c"(0u));
        if (a != 0) {
            return (uint64_t)a * 1000000ull;
        }
    }
    return 0;
}

// Calibrate the TSC against the ACPI PM timer — a firmware-standard 3.579545
// MHz counter that (unlike the legacy PIT) needs no interrupt and is present on
// any ACPI system. Measure the TSC across ~10 ms of PM-timer ticks, handling
// the 24-/32-bit wrap. Returns 0 if the FADT describes no PM timer.
static uint64_t tsc_hz_from_pm_timer(void)
{
    bool is32 = false;
    uint16_t port = acpi_pm_timer_port(&is32);
    if (port == 0) {
        return 0;
    }
    uint32_t mask = is32 ? 0xFFFFFFFFu : 0x00FFFFFFu;
    uint32_t target = PM_TIMER_HZ / 100; // ~10 ms of PM-timer ticks
    uint32_t last = inl(port) & mask;
    uint64_t start_tsc = rdtsc();
    uint64_t guard = start_tsc + 20000000000ull; // bound (~4-6 s)
    uint32_t elapsed = 0;
    while (elapsed < target) {
        uint32_t now = inl(port) & mask;
        elapsed += (now - last) & mask; // wrap-safe delta
        last = now;
        if (rdtsc() > guard) {
            return 0;
        }
    }
    return (rdtsc() - start_tsc) * PM_TIMER_HZ / elapsed;
}

void ktime_init(void)
{
    g_invariant = detect_invariant_tsc();

    // Prefer the PIT-independent CPUID rate; fall back to the ACPI PM timer;
    // and if both fail, assume 1 GHz so the monotonic clock still advances (so
    // millisecond deadlines elsewhere, e.g. the SMP checkin timeout, work).
    g_tsc_hz = tsc_hz_from_cpuid();
    if (g_tsc_hz == 0) {
        g_tsc_hz = tsc_hz_from_pm_timer();
    }
    if (g_tsc_hz == 0) {
        g_tsc_hz = 1000000000ull;
    }
    g_tsc_base = rdtsc();
}

uint64_t tsc_hz(void)
{
    return g_tsc_hz;
}
bool ktime_tsc_invariant(void)
{
    return g_invariant;
}

// Convert elapsed cycles to time units. Split into whole seconds plus a
// remainder so everything stays in 64-bit (cycles * 1e9 would overflow after a
// few seconds, and 128-bit division would need libgcc, which we do not link).
static uint64_t cycles_to(uint64_t cycles, uint64_t per_sec)
{
    if (g_tsc_hz == 0) {
        return 0;
    }
    uint64_t whole = cycles / g_tsc_hz; // whole seconds
    uint64_t rem = cycles % g_tsc_hz;   // leftover cycles (< g_tsc_hz)
    return whole * per_sec + (rem * per_sec) / g_tsc_hz;
}

uint64_t ktime_ns(void)
{
    return cycles_to(rdtsc() - g_tsc_base, 1000000000ull);
}
uint64_t ktime_us(void)
{
    return cycles_to(rdtsc() - g_tsc_base, 1000000ull);
}
uint64_t ktime_ms(void)
{
    return cycles_to(rdtsc() - g_tsc_base, 1000ull);
}
