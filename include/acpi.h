#ifndef __ACPI_H
#define __ACPI_H

#include <stdint.h>
#include <stdbool.h>

// Minimal ACPI support: just enough of the firmware's power tables to shut the
// machine down and reboot it cleanly (the standard way, not a QEMU-only poke).
// We parse the FADT for the PM1 control port and the DSDT for the _S5_
// (soft-off) sleep value; no AML interpreter. Fallbacks cover the case where
// the tables are missing.

// Parse the ACPI tables via uACPI (FADT power control, MADT topology, DSDT _S5
// soft-off value). Requires uacpi_early_tables_init() to have run first; safe
// if the tables are missing (leaves power control on its fallbacks).
void acpi_init(void);

// Power off (ACPI S5). Does not return.
__attribute__((noreturn)) void acpi_shutdown(void);
// Reset the machine. Does not return.
__attribute__((noreturn)) void acpi_reboot(void);

// --- MADT (APIC) topology, parsed from acpi_init -----------------------------

// Physical base of the Local APIC MMIO window (0 if no MADT). Honors a Local
// APIC Address Override entry.
uint64_t acpi_lapic_base(void);

// The first I/O APIC: its MMIO base and the global system interrupt it starts
// at. Returns false if the MADT has none.
bool acpi_ioapic(uint64_t* base, uint32_t* gsi_base);

// Map a legacy ISA IRQ (0-15) to its global system interrupt and MPS INTI flags
// (bits 0-1 polarity, 2-3 trigger), applying MADT Interrupt Source Overrides.
// Identity mapping with default (edge, active-high) flags when no override.
void acpi_irq_to_gsi(uint32_t irq, uint32_t* gsi, uint16_t* flags);

// --- ACPI PM timer (FADT) ----------------------------------------------------

// The ACPI power-management timer I/O port (a 3.579545 MHz free-running
// counter), or 0 if the FADT does not describe one. `*is32bit` reports a 32- vs
// 24-bit counter. A firmware-agnostic time source for calibration (no legacy
// PIT).
uint16_t acpi_pm_timer_port(bool* is32bit);

// --- PCI interrupt routing (_PRT) --------------------------------------------

// Resolve a PCI device's INTx pin (0=INTA..3=INTD) on the root bus to a Global
// System Interrupt via the ACPI _PRT (following link devices). Fills *gsi and
// the MPS INTI *flags (level/active-low for PCI). Returns false if uACPI's
// namespace isn't up or there's no matching entry — callers then fall back to
// the PCI Interrupt Line register. Needs full uACPI init.
bool acpi_pci_route(uint8_t dev, uint8_t pin, uint32_t* gsi, uint16_t* flags);

#endif
