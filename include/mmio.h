#ifndef __MMIO_H
#define __MMIO_H

#include <stdint.h>
#include <barrier.h>

// Typed access to memory-mapped device registers, replacing the
// `*(volatile uint32_t*)(base + off)` casts each driver was open-coding. `base`
// is the mapped register window (from iomap); `off` is a byte offset into it.
// Every access is a single volatile load/store of the exact width.
//
// Not for the LAPIC/IOAPIC: those are x2APIC MSRs / index-data indirect, not
// plain base+offset windows, so they keep their own accessors.

static inline uint8_t mmio_r8(const volatile void* base, uint32_t off)
{
    return *(const volatile uint8_t*)((const volatile uint8_t*)base + off);
}
static inline uint16_t mmio_r16(const volatile void* base, uint32_t off)
{
    return *(const volatile uint16_t*)((const volatile uint8_t*)base + off);
}
static inline uint32_t mmio_r32(const volatile void* base, uint32_t off)
{
    return *(const volatile uint32_t*)((const volatile uint8_t*)base + off);
}
static inline uint64_t mmio_r64(const volatile void* base, uint32_t off)
{
    return *(const volatile uint64_t*)((const volatile uint8_t*)base + off);
}

static inline void mmio_w8(volatile void* base, uint32_t off, uint8_t v)
{
    *(volatile uint8_t*)((volatile uint8_t*)base + off) = v;
}
static inline void mmio_w16(volatile void* base, uint32_t off, uint16_t v)
{
    *(volatile uint16_t*)((volatile uint8_t*)base + off) = v;
}
static inline void mmio_w32(volatile void* base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t*)((volatile uint8_t*)base + off) = v;
}
static inline void mmio_w64(volatile void* base, uint32_t off, uint64_t v)
{
    *(volatile uint64_t*)((volatile uint8_t*)base + off) = v;
}

// Ring a doorbell: order the preceding DMA descriptor/command writes ahead of
// this register store (dma_wmb), then write it. This is the "hand the queued
// entry to the device" step every queue-and-doorbell driver performs.
static inline void mmio_doorbell32(volatile uint32_t* reg, uint32_t v)
{
    dma_wmb();
    *reg = v;
}
static inline void mmio_doorbell16(volatile uint16_t* reg, uint16_t v)
{
    dma_wmb();
    *reg = v;
}

#endif
