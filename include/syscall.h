#ifndef __SYSCALL_H
#define __SYSCALL_H

#include <stdint.h>
#include <stddef.h>

// The int-0x80 syscall boundary for hosted (newlib-linked) programs. These run
// in ring 0 alongside the kernel (the "lab" execution model), but reach kernel
// services — console, heap/sbrk, clock, and later ext2 files — through a trap
// rather than by linking against kernel symbols. libgloss stubs on the program
// side (build/hosted/syscalls.c) issue `int 0x80`; syscall.c implements it.

// Install the int-0x80 handler. Call once from kmain after interrupts_init.
void syscall_init(void);

// Load and run a hosted ELF image in ring 0: give it a heap (for sbrk-based
// malloc), pass argc/argv, jump to its _start, and return main()'s exit status
// (via the exit syscall). Returns -1 if the image isn't a hosted ELF or another
// hosted program is already running (no nesting). BSP-only, synchronous.
int hosted_run(const void* image, size_t size, int argc, char** argv);

#endif
