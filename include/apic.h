#ifndef __APIC_H
#define __APIC_H

#include <stdint.h>
#include <stdbool.h>

// Local APIC + I/O APIC — the modern interrupt hardware that replaces the
// legacy 8259 PIC/PIT. apic_init() brings up the Local APIC (x2APIC when the
// CPU supports it, else xAPIC MMIO); lapic_timer_start() calibrates and starts
// the periodic per-core timer (vector 32); lapic_eoi() signals
// end-of-interrupt. The I/O APIC (ioapic_*, added with the PIC removal) routes
// device IRQ lines to LAPIC vectors. Everything is discovered from ACPI/CPUID,
// never hardcoded.

void apic_init(void);    // bring up + software-enable the Local APIC
void lapic_eoi(void);    // signal end-of-interrupt to the LAPIC
uint32_t lapic_id(void); // this core's Local APIC id
void lapic_timer_start(uint32_t hz); // periodic LAPIC timer -> vector 32

// I/O APIC (routes external device IRQs). ioapic_init() must run after
// apic_init.
void ioapic_init(void);
// Route global system interrupt `gsi` to `vector` on core `dest_apic_id`, using
// the MPS INTI `flags` (polarity/trigger); unmasked. ioapic_mask() disables
// one.
void ioapic_route(uint32_t gsi, uint8_t vector, uint32_t dest_apic_id,
                  uint16_t flags);
void ioapic_mask(uint32_t gsi);

#endif
