#ifndef __BARRIER_H
#define __BARRIER_H

// The three low-level ordering / spin primitives the drivers kept open-coding
// as raw inline asm. Naming them puts the intent in one place instead of a
// comment beside every `__asm__ __volatile__(...)`.

// Compiler-only reordering fence: stops the compiler moving memory accesses
// across this point. No CPU instruction. Use when only compiler ordering
// matters (x86 already keeps stores in program order).
static inline void compiler_barrier(void)
{
    __asm__ __volatile__("" ::: "memory");
}

// DMA write barrier: make prior stores to DMA memory globally visible before a
// following store — specifically the descriptor/command writes that must land
// before the doorbell that hands them to the device. sfence covers write-
// combining mappings too, so it is correct regardless of how the ring is
// mapped (a plain compiler barrier suffices for UC doorbells on x86, but this
// documents the requirement and stays correct if a ring becomes WC).
static inline void dma_wmb(void)
{
    __asm__ __volatile__("sfence" ::: "memory");
}

// Spin-wait relaxation hint (PAUSE): yields the pipeline in a busy-wait so a
// polling loop doesn't hammer the core / bus.
static inline void cpu_relax(void)
{
    __asm__ __volatile__("pause");
}

#endif
