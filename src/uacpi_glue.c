// uACPI integration shim (see uacpi_glue.h). Provides the kernel callbacks the
// vendored uACPI needs in barebones (tables-only) mode, plus early table-access
// setup and a boot-time report. Every physical address uACPI asks us to map is
// already reachable through the Limine HHDM, so map/unmap are trivial.

#include <uacpi_glue.h>
#include <uacpi/kernel_api.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>
#include <uacpi/uacpi.h>

#include <paging.h> // phys_to_virt, hhdm_offset
#include <console.h>

static uacpi_phys_addr g_rsdp; // physical address of the RSDP

// --- kernel callbacks required by barebones uACPI ---------------------------

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out_rsdp_address)
{
    if (g_rsdp == 0) {
        return UACPI_STATUS_NOT_FOUND;
    }
    *out_rsdp_address = g_rsdp;
    return UACPI_STATUS_OK;
}

// All physical memory is identity-mapped through the HHDM, so a "mapping" is
// just the offset view; there is nothing to tear down.
void* uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
    (void)len;
    return phys_to_virt((uintptr_t)addr);
}
void uacpi_kernel_unmap(void* addr, uacpi_size len)
{
    (void)addr;
    (void)len;
}

// uACPI pre-formats the message (barebones/non-formatted logging) and appends a
// newline, so we just emit it. Prefix so it's clear in the boot log.
void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* msg)
{
    (void)level;
    console_print("uacpi: ");
    console_print(msg);
}

// --- setup + report ---------------------------------------------------------

// A scratch buffer uACPI uses to track table descriptors before the heap is
// handed over (barebones early access does no dynamic allocation).
static uint8_t early_table_buf[4096];

bool uacpi_early_tables_init(uint64_t rsdp_addr)
{
    if (rsdp_addr == 0) {
        return false;
    }
    // Limine may hand us the RSDP as an HHDM virtual address; uACPI wants the
    // physical one.
    g_rsdp = rsdp_addr >= hhdm_offset ? rsdp_addr - hhdm_offset : rsdp_addr;

    uacpi_status st = uacpi_setup_early_table_access(early_table_buf,
                                                    sizeof early_table_buf);
    return st == UACPI_STATUS_OK;
}

void uacpi_report(void)
{
    struct acpi_fadt* fadt = NULL;
    uacpi_table madt;
    bool have_fadt = uacpi_table_fadt(&fadt) == UACPI_STATUS_OK && fadt != NULL;
    bool have_madt =
            uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &madt) ==
            UACPI_STATUS_OK;
    console_printf("juampiOS: uacpi v%d.%d.%d tables: FADT=%s MADT=%s\n",
                   UACPI_MAJOR, UACPI_MINOR, UACPI_PATCH,
                   have_fadt ? "ok" : "-", have_madt ? "ok" : "-");
}
