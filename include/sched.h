#ifndef __SCHED_H
#define __SCHED_H

#include <types.h>

// Minimal cooperative kernel-thread scheduler built on the software context
// switch (context.asm). This is the long-mode replacement for the 32-bit
// hardware-TSS task switch; full user-mode processes (ELF, user GDT/TSS,
// syscalls) are layered on top in later milestones.

// Register the currently-running boot context as thread 0.
void sched_init(void);
// Create a kernel thread that begins executing `entry`. Returns its id.
int thread_create(void (*entry)(void));
// Yield the CPU to the next thread in round-robin order.
void yield(void);

#endif
