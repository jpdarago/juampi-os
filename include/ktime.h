#ifndef __KTIME_H
#define __KTIME_H

#include <stdint.h>
#include <stdbool.h>

// High-precision timekeeping via the CPU time-stamp counter (TSC), calibrated
// against the 100 Hz PIT at boot. rdtsc() is the raw cycle counter for
// profiling/benchmarking; ktime_ns/us/ms give a monotonic wall clock.

// Raw TSC read, with an lfence so earlier loads complete first (steadier
// measurements for micro-benchmarks).
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi)::"memory");
    return ((uint64_t)hi << 32) | lo;
}

// Calibrate the TSC frequency against the PIT. Requires interrupts enabled and
// the PIT timer running (call after interrupts_init + sti).
void ktime_init(void);

uint64_t tsc_hz(void);          // calibrated TSC frequency, Hz
bool ktime_tsc_invariant(void); // does the CPU advertise an invariant TSC?

uint64_t ktime_ns(void); // monotonic nanoseconds since ktime_init
uint64_t ktime_us(void);
uint64_t ktime_ms(void);

#endif
