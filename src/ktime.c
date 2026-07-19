#include <ktime.h>
#include <idt.h> // timer_ticks (100 Hz PIT)

#define PIT_HZ 100   // interrupts.c programs the PIT at ~100 Hz
#define CAL_TICKS 10 // calibrate over 10 ticks (~100 ms)

static uint64_t g_tsc_hz;
static uint64_t g_tsc_base;
static bool g_invariant;

static bool detect_invariant_tsc(void)
{
    uint32_t eax, ebx, ecx, edx;
    // CPUID leaf 0x80000007, EDX bit 8 = invariant TSC. Guard on the max
    // extended leaf first.
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(0x80000000));
    if (eax < 0x80000007) {
        return false;
    }
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(0x80000007));
    return (edx & (1u << 8)) != 0;
}

void ktime_init(void)
{
    g_invariant = detect_invariant_tsc();

    // Measure the TSC across a whole number of PIT ticks. Aligning to a tick
    // edge first makes the interval an exact multiple of the (PIT-accurate)
    // tick period, so quantization is not a source of error.
    uint64_t t0 = timer_ticks();
    while (timer_ticks() == t0) {
        __asm__ __volatile__("pause");
    }
    uint64_t start_tick = timer_ticks();
    uint64_t start_tsc = rdtsc();
    while (timer_ticks() - start_tick < CAL_TICKS) {
        __asm__ __volatile__("pause");
    }
    uint64_t end_tsc = rdtsc();
    uint64_t ticks = timer_ticks() - start_tick;

    g_tsc_hz = (end_tsc - start_tsc) * PIT_HZ / ticks;
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
