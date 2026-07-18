#ifndef __SERIAL_H
#define __SERIAL_H

#include <types.h>

// COM1 16550 UART. serial_init() sets it to 115200 8N1; the output helpers are
// polled (no interrupts). Useful for headless boot logging under QEMU
// (`-serial stdio`), which is far friendlier for CI than the VGA text buffer.
void serial_init(void);
void serial_putc(char c);
void serial_print(const char* s);
// Unformatted number helpers (the formatted printf lives in the not-yet-ported
// scrn/vargs code); used for boot logging and fault dumps.
void serial_dec(uint64 v);
void serial_hex(uint64 v);

// Base port and register offsets.
#define SERIAL_COM1 0x3F8
#define SERIAL_THR 0 // transmit holding register (write)
#define SERIAL_IER 1 // interrupt enable
#define SERIAL_FCR 2 // FIFO control (write)
#define SERIAL_LCR 3 // line control
#define SERIAL_MCR 4 // modem control
#define SERIAL_LSR 5 // line status

#define SERIAL_LSR_THR_EMPTY 0x20

#endif
