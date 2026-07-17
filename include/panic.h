#ifndef __PANIC_H
#define __PANIC_H

#include <serial.h>

// Port-era kernel panic: log to the serial console and halt. The formatted,
// VGA-based kernel_panic in exception.h returns once scrn/vargs are ported; in
// the meantime the format arguments are dropped (panics are a rare, terminal
// path where the literal message is enough to locate the fault).
#define kernel_panic(m, ...)                                                   \
    do {                                                                       \
        serial_print("\nKERNEL PANIC (" __FILE__ "): " m "\n");                \
        __asm__ __volatile__("cli");                                           \
        for (;;) {                                                             \
            __asm__ __volatile__("hlt");                                       \
        }                                                                      \
    } while (0)

#endif
