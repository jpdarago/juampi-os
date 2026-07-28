#ifndef __SCHED_H
#define __SCHED_H

#include <alloc.h>

#include <stdint.h>

// Minimal cooperative kernel-thread scheduler built on the software context
// switch (context.asm). This is the long-mode replacement for the 32-bit
// hardware-TSS task switch; full user-mode processes (ELF, user GDT/TSS,
// syscalls) are layered on top in later milestones.

// Size of a kernel stack (16 KiB): one per scheduler thread (sched.c), the TSS
// ring-0 stack (gdt64.c), and each AP's fault/interrupt stack (smp.c).
#define KERNEL_STACK_SZ 0x4000

// Register the currently-running boot context as thread 0; `mem` backs the
// per-thread FPU save areas.
void sched_init(allocator* mem);
// Create a kernel thread that begins executing `entry`, with its stack taken
// from `mem`. Returns its id.
int thread_create(allocator* mem, void (*entry)(void));
// Yield the CPU to the next thread in round-robin order.
void yield(void);

#endif
