#include <serial.h>
#include <ports.h>

void serial_init(void)
{
    outb(SERIAL_COM1 + SERIAL_IER, 0x00); // disable UART interrupts
    outb(SERIAL_COM1 + SERIAL_LCR, 0x80); // enable DLAB to set the baud divisor
    outb(SERIAL_COM1 + 0, 0x01);          // divisor low  -> 115200 baud
    outb(SERIAL_COM1 + 1, 0x00);          // divisor high
    outb(SERIAL_COM1 + SERIAL_LCR, 0x03); // 8 bits, no parity, one stop (8N1)
    outb(SERIAL_COM1 + SERIAL_FCR, 0xC7); // enable + clear FIFOs, 14B threshold
    outb(SERIAL_COM1 + SERIAL_MCR, 0x0B); // DTR, RTS, OUT2
}

static int transmit_empty(void)
{
    return inb(SERIAL_COM1 + SERIAL_LSR) & SERIAL_LSR_THR_EMPTY;
}

void serial_putc(char c)
{
    // Bounded wait: a missing or misbehaving UART must never hang the kernel
    // (a lesson from the ATA driver). If it never drains, we drop the byte.
    for(int i = 0; i < 100000 && !transmit_empty(); i++) {
        ;
    }
    outb(SERIAL_COM1 + SERIAL_THR, c);
}

void serial_print(const char * s)
{
    while(*s) {
        if(*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s);
        s++;
    }
}
