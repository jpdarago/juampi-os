#ifndef __UACPI_GLUE_H
#define __UACPI_GLUE_H

#include <stdint.h>
#include <stdbool.h>

// Integration shim for the vendored uACPI library (src/uacpi/, pinned 6.0.0).
// Currently built in barebones mode — the ACPI *table* subsystem only, no AML
// interpreter — so this provides just the four kernel callbacks uACPI needs in
// that mode (get_rsdp / map / unmap / log) plus early table-access setup. The
// full-init path (namespace, _S5 shutdown, _PRT routing) comes later; see
// docs/acpi-uacpi.md.

// Set the RSDP (from Limine; may be a virtual HHDM address or physical — it is
// normalised to physical) and bring up uACPI early table access. Returns false
// if there is no RSDP or uACPI could not initialise. Safe to call once.
bool uacpi_early_tables_init(uint64_t rsdp_addr);

// Boot report: which standard tables uACPI can see (FADT/MADT), for the log.
void uacpi_report(void);

#endif
