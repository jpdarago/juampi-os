#ifndef __SERIAL_H
#define __SERIAL_H

#include <types.h>

// COM1 16550 UART. serial_init() sets it to 115200 8N1; the output helpers are
// polled (no interrupts). Useful for headless boot logging under QEMU
// (`-serial stdio`), which is far friendlier for CI than the VGA text buffer.
void serial_init(void);
void serial_putc(char c);
void serial_print(const char* s);

// Base port and register offsets, exposed for the loopback self-test.
#define SERIAL_COM1 0x3F8
#define SERIAL_THR 0 // transmit holding register (write)
#define SERIAL_RBR 0 // receive buffer register (read)
#define SERIAL_IER 1 // interrupt enable
#define SERIAL_FCR 2 // FIFO control (write)
#define SERIAL_LCR 3 // line control
#define SERIAL_MCR 4 // modem control
#define SERIAL_LSR 5 // line status

#define SERIAL_LSR_DATA_READY 0x01
#define SERIAL_LSR_THR_EMPTY 0x20
#define SERIAL_MCR_LOOPBACK 0x10

#endif
