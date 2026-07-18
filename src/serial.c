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

int serial_poll(void)
{
    if (!(inb(SERIAL_COM1 + SERIAL_LSR) & SERIAL_LSR_DATA_READY)) {
        return -1;
    }
    return inb(SERIAL_COM1 + SERIAL_RBR);
}

char serial_getc(void)
{
    int c;
    while ((c = serial_poll()) < 0) {
        __asm__ __volatile__("pause");
    }
    return (char)c;
}

void serial_putc(char c)
{
    // Bounded wait: a missing or misbehaving UART must never hang the kernel
    // (a lesson from the ATA driver). If it never drains, we drop the byte.
    for (int i = 0; i < 100000 && !transmit_empty(); i++) {
        ;
    }
    outb(SERIAL_COM1 + SERIAL_THR, c);
}

void serial_print(const char* s)
{
    while (*s) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s);
        s++;
    }
}

void serial_dec(uint64_t v)
{
    char buf[21];
    int i = 20;
    buf[i--] = '\0';
    if (v == 0) {
        buf[i--] = '0';
    }
    while (v > 0) {
        buf[i--] = '0' + (v % 10);
        v /= 10;
    }
    serial_print(&buf[i + 1]);
}

void serial_hex(uint64_t v)
{
    serial_print("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        serial_putc("0123456789abcdef"[(v >> shift) & 0xF]);
    }
}
