// Interrupt handling for the x86-64 port: the C side of the assembly stubs in
// isr.asm. Dispatches CPU exceptions and remapped PIC IRQs, drives the 8259
// PICs and the PIT timer, and reports unhandled faults over the serial console.
// The rich VGA fault screen and the scheduler tick return once scrn and the
// task subsystem are ported.

#include <idt.h>
#include <ports.h>
#include <console.h>
#include <ksym.h>

// 8259 PIC ports and commands.
#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI 0x20
#define ICW1_INIT 0x11 // init + ICW4 present
#define ICW4_8086 0x01

// PIT (8253/8254) ports.
#define PIT_CH0 0x40
#define PIT_CMD 0x43
#define PIT_FREQUENCY 1193182

static interrupt_handler handlers[256];
static volatile uint64_t ticks;

void register_interrupt_handler(uint32_t vector, interrupt_handler h)
{
    if (vector < 256) {
        handlers[vector] = h;
    }
}

uint64_t timer_ticks(void)
{
    return ticks;
}

static void io_wait(void)
{
    outb(0x80, 0);
}

static void pic_remap(void)
{
    // Start init, remap master to vectors 0x20-0x27 and slave to 0x28-0x2F so
    // the IRQs no longer collide with the CPU exception vectors.
    outb(PIC1_CMD, ICW1_INIT);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT);
    io_wait();
    outb(PIC1_DATA, 0x20);
    io_wait();
    outb(PIC2_DATA, 0x28);
    io_wait();
    outb(PIC1_DATA, 0x04); // tell master: slave at IRQ2
    io_wait();
    outb(PIC2_DATA, 0x02); // tell slave: cascade identity
    io_wait();
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();
    // Mask everything for now; interrupts_init unmasks the timer.
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

static void pic_eoi(uint32_t irq)
{
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

void irq_unmask(uint32_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = 1 << (irq & 7);
    outb(port, inb(port) & ~bit);
}

static void pit_init(uint32_t hz)
{
    uint32_t divisor = PIT_FREQUENCY / hz;
    outb(PIT_CMD, 0x36); // channel 0, lobyte/hibyte, mode 3 (square wave)
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, (divisor >> 8) & 0xFF);
}

static void timer_handler(interrupt_frame* f)
{
    (void)f;
    ticks++;
}

// Unhandled CPU exception: dump the frame to serial and halt.
static void exception_panic(interrupt_frame* f)
{
    uint64_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    console_print("\n*** CPU EXCEPTION ***\n  vector=");
    console_dec(f->vector);
    console_print(" error=");
    console_hex(f->error_code);
    console_print("\n  rip=");
    console_hex(f->rip);
    console_print(" cs=");
    console_hex(f->cs);
    console_print(" rflags=");
    console_hex(f->rflags);
    console_print("\n  rsp=");
    console_hex(f->rsp);
    console_print(" cr2=");
    console_hex(cr2);
    console_print("\n");
    // Symbolized backtrace of the faulting context (rbp saved in the frame).
    const char* fn = ksym_lookup(f->rip, NULL);
    if (fn) {
        console_print("  in ");
        console_print(fn);
        console_print("\n");
    }
    backtrace_from(f->rip, f->rbp);
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

// Called from the assembly stubs with the saved register frame.
void interrupt_dispatch(interrupt_frame* f)
{
    interrupt_handler h = handlers[f->vector];

    if (f->vector >= 32 && f->vector < 48) {
        if (h) {
            h(f);
        }
        pic_eoi(f->vector - 32);
        return;
    }

    if (h) {
        h(f);
        return;
    }

    // Unhandled exception (vector < 32) or stray vector: fatal.
    exception_panic(f);
}

void interrupts_init(void)
{
    idt_init();
    pic_remap();
    pit_init(100); // ~100 Hz
    register_interrupt_handler(32, timer_handler);
    outb(PIC1_DATA, 0xFE); // unmask IRQ0 (timer) only
    outb(PIC2_DATA, 0xFF);
}
