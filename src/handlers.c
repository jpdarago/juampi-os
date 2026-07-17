#include <irq.h>
#include <idt.h>
#include <utils.h>
#include <scrn.h>
#include <timer.h>
#include <exception.h>
#include <keyboard.h>

// Pointers to the interrupt service functions.
static irq_handler irq_handlers[MAX_INTS];

// Sends the messages to remap the PIC.
void remap_pic()
{
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}

static void irq_unknown_handler(uint irq_code, gen_regs regs)
{
    uchar row = scrn_getrow(), col = scrn_getcol();
    scrn_setcursor(VIDEO_HEIGHT - 1, 0);
    scrn_printf("Unknown interrupt %u", irq_code);
    scrn_setcursor(row, col);
}

// Registers a handler for an interrupt
void register_irq_handler(irq_handler ih, uint code)
{
    if (code < MAX_INTS) {
        irq_handlers[code] = ih;
    } else {
        kernel_panic("Invalid interrupt");
    }
}

void irq_init_handlers()
{
    memset(irq_handlers, 0, sizeof(irq_handlers));
    init_timer(1000);
    uint i;

    for (i = 0; i < MAX_INTS; i++) {
        irq_handlers[i] = irq_unknown_handler;
    }

    register_irq_handler(schedule, 0x20);
    register_irq_handler(keyboard_irq_handler, 0x21);
}

#define IF_BIT 9
inline void irq_sti(uint eflags)
{
    // If interrupts were originally enabled we enable
    // them again
    if (eflags & (1 << IF_BIT)) {
        __asm__ __volatile__("sti");
    }
}

inline void irq_sti_force()
{
    __asm__ __volatile__("sti");
}

inline uint irq_cli()
{
    uint eflags;
    __asm__ __volatile__("pushf\n\t"
                         "pop %%eax\n\t"
                         "mov %%eax, %0"
                         : "=r"(eflags)::"eax");
    __asm__ __volatile__("cli");
    return eflags;
}

// Common handler for hardware interrupts: These come from
// the PIC pins of the CPU
void irq_common_handler(gen_regs regs, uint irq_code, int_trace trace)
{
    if (irq_code == 7 + 32) {
        // Spurious interrupt. If the real interrupt
        // bit 7 is not set, we return.
        outb(0x20, 0x0B);
        uchar irr = inb(0x20);
        if (!(irr & 0x80)) {
            return;
        }
    }
    if (irq_code >= 40) {
        // We tell the master PIC that we processed the interrupt.
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);

    if (irq_handlers[irq_code]) {
        // We invoke the real handler
        irq_handler handler = irq_handlers[irq_code];
        handler(irq_code, regs);
    }
}
