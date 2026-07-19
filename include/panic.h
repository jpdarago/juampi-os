#ifndef __PANIC_H
#define __PANIC_H

#include <console.h>
#include <ksym.h>

// Kernel panic: log to the console, print a symbolized backtrace, and halt. The
// format arguments are dropped (panics are a rare, terminal path where the
// literal message plus the backtrace is enough to locate the fault).
#define kernel_panic(m, ...)                                                   \
    do {                                                                       \
        console_print("\nKERNEL PANIC (" __FILE__ "): " m "\n");               \
        backtrace();                                                           \
        __asm__ __volatile__("cli");                                           \
        for (;;) {                                                             \
            __asm__ __volatile__("hlt");                                       \
        }                                                                      \
    } while (0)

#endif
