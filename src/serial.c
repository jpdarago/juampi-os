#include <barrier.h>
#include <serial.h>
#include <ports.h>

// 16550 UART configuration values written at init.
#define LCR_DLAB 0x80            // Line Control: divisor-latch access
#define LCR_8N1 0x03             // Line Control: 8 data bits, no parity, 1 stop
#define FCR_ENABLE_CLEAR_14 0xC7 // FIFO: enable + clear RX/TX, 14-byte trigger
#define MCR_DTR_RTS_OUT2 0x0B    // Modem: DTR + RTS + OUT2 (OUT2 gates the IRQ)
#define BAUD_DIVISOR_115200 1    // 115200 = 115200 / 1

void serial_init(void)
{
    outb(SERIAL_COM1 + SERIAL_IER, 0x00);       // disable UART interrupts
    outb(SERIAL_COM1 + SERIAL_LCR, LCR_DLAB);   // DLAB: set the baud divisor
    outb(SERIAL_COM1 + 0, BAUD_DIVISOR_115200); // divisor low
    outb(SERIAL_COM1 + 1, 0x00);                // divisor high
    outb(SERIAL_COM1 + SERIAL_LCR, LCR_8N1);
    outb(SERIAL_COM1 + SERIAL_FCR, FCR_ENABLE_CLEAR_14);
    outb(SERIAL_COM1 + SERIAL_MCR, MCR_DTR_RTS_OUT2);
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

void serial_putc(char c)
{
    // Bounded wait: a missing or misbehaving UART must never hang the kernel
    // (a lesson from the ATA driver). If it never drains, we drop the byte.
    for (int i = 0; i < 100000 && !transmit_empty(); i++) {
        ;
    }
    outb(SERIAL_COM1 + SERIAL_THR, c);
}
