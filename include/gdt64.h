#ifndef __GDT64_H
#define __GDT64_H

#include <types.h>

// Segment selectors in the port's 64-bit GDT (RPL added where used).
#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UCODE 0x18 // | 3 for ring-3 use
#define GDT_UDATA 0x20 // | 3 for ring-3 use
#define GDT_TSS 0x28

// 64-bit Task State Segment. In long mode the TSS no longer drives task
// switching; it only supplies the stack pointers used on a privilege change
// (rsp0) and the IST stacks.
typedef struct {
    uint32 reserved0;
    uint64 rsp0, rsp1, rsp2;
    uint64 reserved1;
    uint64 ist[7];
    uint64 reserved2;
    uint16 reserved3;
    uint16 iomap_base;
} __attribute__((__packed__)) tss64;

// Install and load the GDT (kernel + user code/data) and the TSS, and reload
// the segment registers. Must run before idt_init so the IDT gates reference
// the GDT's kernel code selector.
void gdt_init(void);

// Set the kernel stack the CPU switches to when an interrupt enters ring 0 from
// ring 3.
void tss_set_rsp0(uint64 rsp0);

// Assembly helpers (gdt64.asm).
void gdt_flush(void* gdtr);      // lgdt + reload CS/DS/SS to kernel selectors
void tss_flush(uint16 selector); // ltr
void enter_user_mode(uint64 rip,
                     uint64 rsp); // iretq into ring 3 (never returns)

#endif
