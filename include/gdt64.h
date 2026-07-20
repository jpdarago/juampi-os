#ifndef __GDT64_H
#define __GDT64_H

#include <alloc.h>

#include <stdint.h>

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
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((__packed__)) tss64;

// Install and load the GDT (kernel + user code/data) and the TSS, and reload
// the segment registers; the ring-0 interrupt stack comes from `mem`. Must run
// before idt_init so the IDT gates reference the GDT's kernel code selector.
void gdt_init(allocator* mem);

// Set the kernel stack the CPU switches to when an interrupt enters ring 0 from
// ring 3.
void tss_set_rsp0(uint64_t rsp0);

// Build and load a per-CPU GDT `g` and TSS `t` (with interrupt stack `rsp0`) on
// the calling core. Used to give each application processor its own GDT/TSS
// while sharing the same descriptor layout as the BSP.
void gdt_ap_load(uint64_t g[7], tss64* t, uint64_t rsp0);

// Assembly helpers (gdt64_load.asm).
void gdt_flush(void* gdtr);        // lgdt + reload CS/DS/SS to kernel selectors
void tss_flush(uint16_t selector); // ltr
void enter_user_mode(uint64_t rip,
                     uint64_t rsp); // iretq into ring 3 (never returns)

#endif
