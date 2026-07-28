// Interrupt handling for the x86-64 port: the C side of the assembly stubs in
// isr.asm. Dispatches CPU exceptions and device IRQs, and reports unhandled
// faults over the serial console. Interrupts run on the modern APIC
// (src/apic.c): the Local APIC timer drives the tick, the I/O APIC routes
// device lines to vectors, and EOI goes to the LAPIC. The legacy 8259 PIC is
// fully masked off.

#include <idt.h>
#include <ports.h>
#include <console.h>
#include <ksym.h>
#include <fault.h>
#include <apic.h>
#include <acpi.h>

// The spurious-interrupt vector (LAPIC SVR); reuses stub 0x2F (IRQ15, unused).
// It takes no EOI.
#define SPURIOUS_VECTOR 0x2F

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

static void timer_handler(interrupt_frame* f)
{
    (void)f;
    ticks++;
}

static void spurious_handler(interrupt_frame* f)
{
    (void)f; // a LAPIC spurious interrupt: nothing to do, and no EOI
}

// Route a legacy ISA IRQ line (0-15) to its vector (32 + irq) on the BSP
// through the I/O APIC, applying the ACPI interrupt-source override. Replaces
// the old PIC unmask; keyboard.c / mouse.c call this unchanged.
void irq_unmask(uint32_t irq)
{
    uint32_t gsi;
    uint16_t flags;
    acpi_irq_to_gsi(irq, &gsi, &flags);
    ioapic_route(gsi, (uint8_t)(32 + irq), lapic_id(), flags);
}

// Fully disable the legacy 8259 PIC by masking all of its lines (so it can't
// deliver a stray interrupt now that the APIC is in charge).
static void pic_disable(void)
{
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

// Unhandled CPU exception. If the shell has armed fault recovery (it is
// evaluating a script), unwind back to the prompt; otherwise this is a kernel
// bug — dump the frame with a backtrace and halt.
static void exception_panic(interrupt_frame* f)
{
    if (fault_recover(f)) {
        return; // (unreachable — fault_recover longjmps when armed)
    }
    uint64_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    console_printf("\n*** CPU EXCEPTION ***\n  vector=%lu error=0x%lx\n"
                   "  rip=0x%lx cs=0x%lx rflags=0x%lx\n  rsp=0x%lx cr2=0x%lx\n",
                   f->vector, f->error_code, f->rip, f->cs, f->rflags, f->rsp,
                   cr2);
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
        // The LAPIC spurious vector must NOT be acknowledged; everything else
        // (timer + IOAPIC-routed device IRQs) EOIs to the Local APIC.
        if (f->vector != SPURIOUS_VECTOR) {
            lapic_eoi();
        }
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
    pic_disable(); // retire the legacy 8259 before enabling the APIC
    apic_init();   // Local APIC (x2APIC/xAPIC) — the tick starts in kmain
    ioapic_init(); // I/O APIC — irq_unmask() routes device lines through it
    register_interrupt_handler(SPURIOUS_VECTOR, spurious_handler);
    register_interrupt_handler(32, timer_handler);
}
