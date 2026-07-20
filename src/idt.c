#include <idt.h>

static idt_entry idt[256];
static idt_ptr idtr;

static void idt_set(uint32_t vector, uintptr_t handler, uint16_t selector,
                    uint8_t type_attr)
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
    uint16_t cs;
    __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));

    for (uint32_t v = 0; v < 256; v++) {
        idt_set(v, isr_stub_table[v], cs, IDT_GATE_KERNEL);
    }
    // The int 0x80 syscall gate must be reachable from ring 3.
    idt_set(0x80, isr_stub_table[0x80], cs, IDT_GATE_USER);

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uintptr_t)&idt;
    __asm__ __volatile__("lidt %0" ::"m"(idtr));
}

void idt_load(void)
{
    // Load the already-built IDT on the calling core (used by APs; the one IDT
    // is shared by all cores).
    __asm__ __volatile__("lidt %0" ::"m"(idtr));
}
