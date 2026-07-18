#ifndef __ISR_H
#define __ISR_H

#include <types.h>

// x86-64 IDT gate descriptor: 16 bytes, with the 64-bit handler offset split
// across three fields and an IST selector.
typedef struct {
    uint16 offset_low;
    uint16 selector;
    uint8 ist;       // bits 0-2: interrupt-stack-table index (0 = none)
    uint8 type_attr; // present | DPL | gate type
    uint16 offset_mid;
    uint32 offset_high;
    uint32 zero;
} __attribute__((__packed__)) idt_entry;

typedef struct {
    uint16 limit;
    uint64 base;
} __attribute__((__packed__)) idt_ptr;

// Gate type_attr values: present (0x80) | DPL | 0xE (64-bit interrupt gate).
#define IDT_GATE_KERNEL 0x8E // present, DPL 0, interrupt gate
#define IDT_GATE_USER 0xEE   // present, DPL 3 (allows int from user, e.g. 0x80)

// The register frame the assembly stubs build on the stack and hand to
// interrupt_dispatch. The general-purpose registers are in push order; vector
// and error_code are pushed by the stubs; the rest is the CPU interrupt frame.
typedef struct {
    uint64 r15, r14, r13, r12, r11, r10, r9, r8;
    uint64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64 vector, error_code;
    uint64 rip, cs, rflags, rsp, ss;
} interrupt_frame;

typedef void (*interrupt_handler)(interrupt_frame*);

// Build and load the IDT, wiring vectors 0-47 to the assembly stubs.
void idt_init(void);
// Register a C handler for an interrupt vector (0-255).
void register_interrupt_handler(uint vector, interrupt_handler h);
// Bring up the IDT, the 8259 PICs and the PIT timer (IRQ0 @ ~100 Hz).
void interrupts_init(void);
// Timer ticks since boot.
uint64 timer_ticks(void);

// The 48 assembly entry stubs (exceptions 0-31, IRQs 32-47), as an address
// table indexed by vector.
extern uintptr isr_stub_table[];

#endif
