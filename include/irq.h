#ifndef __INTERRUPTS_H
#define __INTERRUPTS_H

#include <scrn.h>
#include <types.h>
#include <idt.h>
#include <ports.h>
#include <proc.h>

// Type of the callbacks
typedef void (*irq_handler)(uint, gen_regs);

// Remapping of the PIC
extern void remap_pic(void);

// Load the irq handlers and friends
extern void irq_init_handlers(void);

// General handler for interrupts via PIC
extern void irq_common_handler(gen_regs, uint, int_trace);

// Handlers to turn interrupts on and off
// irq_sti decides whether it has to enable them or not according to eflags
extern void irq_sti(uint eflags);
// Enables interrupts unconditionally
extern void irq_sti_force(void);
// Disables interrupts, returns the previous eflags
extern uint irq_cli(void);

// Register handler
extern void register_irq_handler(irq_handler e, uint code);

// Maximum number of interrupts that the system handles
#define MAX_INTS 64

#endif
