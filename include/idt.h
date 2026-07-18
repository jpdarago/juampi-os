#ifndef __ISR_H
#define __ISR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// x86-64 IDT gate descriptor: 16 bytes, with the 64-bit handler offset split
// across three fields and an IST selector.
typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;       // bits 0-2: interrupt-stack-table index (0 = none)
    uint8_t type_attr; // present | DPL | gate type
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((__packed__)) idt_entry;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((__packed__)) idt_ptr;

// Gate type_attr values: present (0x80) | DPL | 0xE (64-bit interrupt gate).
#define IDT_GATE_KERNEL 0x8E // present, DPL 0, interrupt gate
#define IDT_GATE_USER 0xEE   // present, DPL 3 (allows int from user, e.g. 0x80)

// The register frame the assembly stubs build on the stack and hand to
// interrupt_dispatch. The general-purpose registers are in push order; vector
// and error_code are pushed by the stubs; the rest is the CPU interrupt frame.
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} interrupt_frame;

typedef void (*interrupt_handler)(interrupt_frame*);

// Build and load the IDT, wiring vectors 0-47 to the assembly stubs.
void idt_init(void);
// Register a C handler for an interrupt vector (0-255).
void register_interrupt_handler(uint32_t vector, interrupt_handler h);
// Bring up the IDT, the 8259 PICs and the PIT timer (IRQ0 @ ~100 Hz).
void interrupts_init(void);
// Timer ticks since boot.
uint64_t timer_ticks(void);

// The 48 assembly entry stubs (exceptions 0-31, IRQs 32-47), as an address
// table indexed by vector.
extern uintptr_t isr_stub_table[];

#endif
