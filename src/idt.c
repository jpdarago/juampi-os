#include <idt.h>
#include <utils.h>
#include <gdt.h>

idt_entry idt[256];
idt_desc IDT_DESC = {.idt_base = (intptr)& idt, .idt_limit = sizeof(idt)-1};

void idt_load_desc(uint i, uint offset, ushort sel, idt_entry_flags flags)
{
    idt[i].offset_l = (ushort)(offset & 0xFFFF);
    idt[i].offset_h = (ushort)((offset >> 16) & 0xFFFF);
    idt[i].selector = sel;
    idt[i].__padding__ = 0;
    idt[i].type = flags.type | (flags.d << 3);
    idt[i].dpl = flags.dpl;
    idt[i].p = 1;
}
#define IDT_LOAD_DESC(i,o,s,...) \
    idt_load_desc(i,o,s,(idt_entry_flags) { \
                      .dpl = 0, .d = 1, .type = IDT_INT_GATE, __VA_ARGS__ })

static intptr excph_addresses[] = {
    (intptr)& _isr0, (intptr)& _isr1, (intptr)& _isr2, (intptr)& _isr3, (intptr)& _isr4,
    (intptr)& _isr5, (intptr)& _isr6, (intptr)& _isr7, (intptr)& _isr8, (intptr)& _isr9,
    (intptr)& _isr10, (intptr)& _isr11, (intptr)& _isr12, (intptr)& _isr13, (intptr)& _isr14,
    (intptr)& _isr15, (intptr)& _isr16, (intptr)& _isr17, (intptr)& _isr18, (intptr)& _isr19
};

void idt_init_exceptions()
{
    memset((uchar*)idt,0,sizeof(idt));
    for(uint i = 0; i < 19; i++) {
        IDT_LOAD_DESC(i,excph_addresses[i],CODE_SEGMENT_KERNEL);
    }
}

static intptr inter_handlers[] = {
    (intptr)& _irq0, (intptr)& _irq1, (intptr)& _irq2, (intptr)& _irq3, (intptr)& _irq4,
    (intptr)& _irq5, (intptr)& _irq6, (intptr)& _irq7, (intptr)& _irq8, (intptr)& _irq9,
    (intptr)& _irq10, (intptr)& _irq11, (intptr)& _irq12, (intptr)& _irq13, (intptr)& _irq14,
    (intptr)& _irq15
};

void idt_init_interrupts()
{
    //Ahora van los handlers de interrupciones por PIC
    for(uint i = 0; i < 16; i++) {
        IDT_LOAD_DESC(32+i, inter_handlers[i], CODE_SEGMENT_KERNEL);
    }
}

void idt_init_syscalls()
{
    IDT_LOAD_DESC(0x80, (intptr) &_isr0x80, CODE_SEGMENT_KERNEL, .dpl = 3);
}
