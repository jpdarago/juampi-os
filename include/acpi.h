#ifndef __ACPI_H
#define __ACPI_H

#include <stdint.h>

// Minimal ACPI support: just enough of the firmware's power tables to shut the
// machine down and reboot it cleanly (the standard way, not a QEMU-only poke).
// We parse the FADT for the PM1 control port and the DSDT for the _S5_
// (soft-off) sleep value; no AML interpreter. Fallbacks cover the case where
// the tables are missing.

// Parse the ACPI tables reachable from the RSDP (Limine gives us its address).
// Safe to call with 0 / a bad pointer (leaves power control on its fallbacks).
void acpi_init(uint64_t rsdp_addr);

// Power off (ACPI S5). Does not return.
__attribute__((noreturn)) void acpi_shutdown(void);
// Reset the machine. Does not return.
__attribute__((noreturn)) void acpi_reboot(void);

#endif
