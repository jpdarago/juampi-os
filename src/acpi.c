#include <acpi.h>
#include <uacpi_glue.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>
#include <paging.h>
#include <ports.h>
#include <utils.h>

#include <stddef.h>

// ACPI support built on the vendored uACPI table subsystem (barebones mode; see
// uacpi_glue.c / docs/acpi-uacpi.md). uACPI locates and validates the tables —
// walking the RSDP/RSDT/XSDT itself is gone — and hands us typed structs. We
// still read what the *tables* directly expose: the FADT (PM1 control port, PM
// timer, reset register), the MADT (LAPIC/IOAPIC topology, interrupt-source
// overrides), and, for soft-off, a byte-scan of the DSDT's _S5_ package. That
// last scan is the one thing that genuinely wants an AML interpreter; it stays
// until uACPI is brought up in full-init mode (then acpi_shutdown moves to
// uacpi_enter_sleep_state). uacpi_early_tables_init() must run before this.

static bool have_pm; // PM1 control port known
static uint16_t pm1a_cnt, pm1b_cnt;
static bool have_s5;
static uint8_t slp_typa, slp_typb;
static uint32_t fadt_flags;
static struct acpi_gas reset_reg;
static uint8_t reset_value;

// MADT (APIC) topology.
static uint64_t madt_lapic_base;
static uint64_t madt_ioapic_base;
static uint32_t madt_ioapic_gsi_base;
static bool madt_have_ioapic;
#define MAX_OVERRIDES 16
static struct {
    uint8_t source; // legacy ISA IRQ
    uint32_t gsi;   // global system interrupt it is routed to
    uint16_t flags; // MPS INTI flags
} madt_overrides[MAX_OVERRIDES];
static int madt_noverrides;

// ACPI PM timer.
static uint16_t pm_timer_port;
static bool pm_timer_32bit;

static bool sig_is(const char* a, const char* b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

// Scan the DSDT for the _S5_ package and pull out SLP_TYPa/b — the standard
// non-AML shutdown recipe. Layout: NameOp(0x08) "_S5_" PackageOp(0x12)
// <pkglength> <numelements> <SLP_TYPa> <SLP_TYPb> ...
static void parse_s5(const struct acpi_sdt_hdr* dsdt)
{
    const uint8_t* s = (const uint8_t*)dsdt + sizeof(struct acpi_sdt_hdr);
    uint32_t len = dsdt->length - (uint32_t)sizeof(struct acpi_sdt_hdr);
    for (uint32_t i = 0; i + 6 < len; i++) {
        if (s[i] != '_' || s[i + 1] != 'S' || s[i + 2] != '5' ||
            s[i + 3] != '_') {
            continue;
        }
        // Preceding NameOp, and a PackageOp right after the name.
        bool named = (i >= 1 && s[i - 1] == 0x08) ||
                     (i >= 2 && s[i - 2] == 0x08 && s[i - 1] == '\\');
        if (!named || s[i + 4] != 0x12) {
            continue;
        }
        const uint8_t* p = s + i + 5; // past "_S5_" and PackageOp
        p += ((*p & 0xC0) >> 6) + 2;  // skip pkglength + numelements
        if (*p == 0x0A) {
            p++; // BytePrefix
        }
        slp_typa = *p++;
        if (*p == 0x0A) {
            p++;
        }
        slp_typb = *p;
        have_s5 = true;
        return;
    }
}

// Walk the MADT entries: record the LAPIC base, the first I/O APIC, and any
// interrupt source overrides. See ACPI spec 5.2.12.
static void parse_madt(const struct acpi_madt* madt)
{
    madt_lapic_base = madt->local_interrupt_controller_address;

    const uint8_t* base = (const uint8_t*)madt;
    uint32_t total = madt->hdr.length;
    uint32_t off = sizeof(struct acpi_madt); // first controller structure
    while (off + sizeof(struct acpi_entry_hdr) <= total) {
        const struct acpi_entry_hdr* e =
                (const struct acpi_entry_hdr*)(base + off);
        if (e->length < sizeof(struct acpi_entry_hdr) ||
            off + e->length > total) {
            break;
        }
        switch (e->type) {
        case ACPI_MADT_ENTRY_TYPE_IOAPIC:
            if (!madt_have_ioapic) {
                const struct acpi_madt_ioapic* io =
                        (const struct acpi_madt_ioapic*)e;
                madt_ioapic_base = io->address;
                madt_ioapic_gsi_base = io->gsi_base;
                madt_have_ioapic = true;
            }
            break;
        case ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE:
            if (madt_noverrides < MAX_OVERRIDES) {
                const struct acpi_madt_interrupt_source_override* ov =
                        (const struct acpi_madt_interrupt_source_override*)e;
                madt_overrides[madt_noverrides].source = ov->source;
                madt_overrides[madt_noverrides].gsi = ov->gsi;
                madt_overrides[madt_noverrides].flags = ov->flags;
                madt_noverrides++;
            }
            break;
        case ACPI_MADT_ENTRY_TYPE_LAPIC_ADDRESS_OVERRIDE: {
            const struct acpi_madt_lapic_address_override* lo =
                    (const struct acpi_madt_lapic_address_override*)e;
            madt_lapic_base = lo->address;
            break;
        }
        default:
            break;
        }
        off += e->length;
    }
}

uint64_t acpi_lapic_base(void)
{
    return madt_lapic_base;
}

bool acpi_ioapic(uint64_t* base, uint32_t* gsi_base)
{
    if (!madt_have_ioapic) {
        return false;
    }
    if (base != NULL) {
        *base = madt_ioapic_base;
    }
    if (gsi_base != NULL) {
        *gsi_base = madt_ioapic_gsi_base;
    }
    return true;
}

void acpi_irq_to_gsi(uint32_t irq, uint32_t* gsi, uint16_t* flags)
{
    for (int i = 0; i < madt_noverrides; i++) {
        if (madt_overrides[i].source == irq) {
            *gsi = madt_overrides[i].gsi;
            *flags = madt_overrides[i].flags;
            return;
        }
    }
    *gsi = irq; // identity mapping for un-overridden ISA IRQs
    *flags = 0; // MPS default: edge-triggered, active-high
}

uint16_t acpi_pm_timer_port(bool* is32bit)
{
    if (is32bit != NULL) {
        *is32bit = pm_timer_32bit;
    }
    return pm_timer_port;
}

void acpi_init(void)
{
    // MADT (interrupt topology).
    uacpi_table madt;
    if (uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &madt) ==
        UACPI_STATUS_OK) {
        parse_madt((const struct acpi_madt*)madt.ptr);
    }

    // FADT (power management): uACPI has already located and validated it.
    struct acpi_fadt* f = NULL;
    if (uacpi_table_fadt(&f) != UACPI_STATUS_OK || f == NULL) {
        return;
    }
    pm1a_cnt = (uint16_t)f->pm1a_cnt_blk;
    pm1b_cnt = (uint16_t)f->pm1b_cnt_blk;
    fadt_flags = f->flags;
    reset_reg = f->reset_reg;
    reset_value = f->reset_value;
    have_pm = pm1a_cnt != 0;

    // ACPI PM timer: PM_TMR_BLK is an I/O port; the counter is 32-bit when FADT
    // flags bit 8 (TMR_VAL_EXT) is set. Prefer the extended X_PM_TMR_BLK GAS
    // when it is present and I/O-mapped.
    pm_timer_port = (uint16_t)f->pm_tmr_blk;
    pm_timer_32bit = (fadt_flags & (1u << 8)) != 0;
    if (f->hdr.length >= offsetof(struct acpi_fadt, x_pm_tmr_blk) +
                                 sizeof(struct acpi_gas) &&
        f->x_pm_tmr_blk.address_space_id == 1 && f->x_pm_tmr_blk.address != 0) {
        pm_timer_port = (uint16_t)f->x_pm_tmr_blk.address;
    }

    // DSDT (for the _S5_ soft-off recipe). uACPI registered it from the FADT.
    uacpi_table dsdt;
    if (uacpi_table_find_by_signature(ACPI_DSDT_SIGNATURE, &dsdt) ==
                UACPI_STATUS_OK &&
        sig_is(dsdt.hdr->signature, "DSDT", 4)) {
        parse_s5(dsdt.hdr);
    }
}

#define SLP_EN (1u << 13)

void acpi_shutdown(void)
{
    // Prefer AML-evaluated S5 (uACPI full init). Returns only if unavailable,
    // then we fall back to the byte-scanned _S5 / PM1 path below.
    uacpi_try_shutdown();
    if (have_pm && have_s5) {
        outw(pm1a_cnt, (uint16_t)(((uint16_t)slp_typa << 10) | SLP_EN));
        if (pm1b_cnt != 0) {
            outw(pm1b_cnt, (uint16_t)(((uint16_t)slp_typb << 10) | SLP_EN));
        }
    }
    // Emulator fallbacks if ACPI S5 didn't take (QEMU PIIX4/ICH9, Bochs, VBox).
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x600, 0x34);
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

void acpi_reboot(void)
{
    // ACPI reset register, if the FADT advertises one (flags bit 10).
    if ((fadt_flags & (1u << 10)) && reset_reg.address != 0) {
        if (reset_reg.address_space_id == 1) {
            outb((uint16_t)reset_reg.address, reset_value);
        } else if (reset_reg.address_space_id == 0) {
            *(volatile uint8_t*)phys_to_virt(reset_reg.address) = reset_value;
        }
    }
    // Pulse the 8042 keyboard controller's CPU reset line.
    uint8_t s = 0x02;
    while (s & 0x02) {
        s = inb(0x64);
    }
    outb(0x64, 0xFE);
    // Last resort: triple fault via a null IDT.
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) nidt = {0, 0};
    __asm__ __volatile__("lidt %0; int3" ::"m"(nidt));
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}
