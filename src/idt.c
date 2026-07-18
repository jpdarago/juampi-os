#include <idt.h>

static idt_entry idt[256];
static idt_ptr idtr;

static void idt_set(uint vector, uintptr handler, uint16 selector,
                    uint8 type_attr)
{
    idt[vector].offset_low = handler & 0xFFFF;
    idt[vector].selector = selector;
    idt[vector].ist = 0;
    idt[vector].type_attr = type_attr;
    idt[vector].offset_mid = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].zero = 0;
}

void idt_init(void)
{
    // Gates use whatever 64-bit code selector we are currently running under
    // (Limine set up the GDT and loaded CS); read it rather than hard-coding.
    uint16 cs;
    __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));

    for (uint v = 0; v < 48; v++) {
        idt_set(v, isr_stub_table[v], cs, IDT_GATE_KERNEL);
    }

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uintptr)&idt;
    __asm__ __volatile__("lidt %0" ::"m"(idtr));
}
