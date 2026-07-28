#include <acpi.h>
#include <paging.h>
#include <ports.h>
#include <utils.h>

#include <stddef.h>

// Read-only ACPI: locate the FADT (PM1 control port) and the DSDT's _S5_ object
// (the soft-off sleep type), so acpi_shutdown() can enter S5 and acpi_reboot()
// can use the ACPI reset register. Table pointers inside ACPI are physical, so
// they are reached through the Limine higher-half direct map.

struct sdt_header {
    char sig[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemid[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct rsdp {
    char sig[8];
    uint8_t checksum;
    char oemid[6];
    uint8_t revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

// ACPI Generic Address Structure (used by the FADT reset register).
struct gas {
    uint8_t address_space; // 0 = system memory, 1 = system I/O
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed));

// Fixed ACPI Description Table (signature "FACP"). Fields laid out at their
// spec offsets so they are read by name instead of by raw byte offset; the
// table on a given machine may be truncated, so length is checked before
// touching the extended (X_*) fields near the end. (ACPI spec 5.2.9.)
struct fadt {
    struct sdt_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    struct gas reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    struct gas x_pm1a_evt_blk;
    struct gas x_pm1b_evt_blk;
    struct gas x_pm1a_cnt_blk;
    struct gas x_pm1b_cnt_blk;
    struct gas x_pm2_cnt_blk;
    struct gas x_pm_tmr_blk;
} __attribute__((packed));

// MADT (signature "APIC") and its variable-length Interrupt Controller
// Structures. Each entry starts with the common {type,length} header; only the
// ones we act on have a full struct. (ACPI spec 5.2.12.)
struct madt {
    struct sdt_header header;
    uint32_t lapic_addr;
    uint32_t flags;
    // Interrupt Controller Structures follow, walked by their length.
} __attribute__((packed));

struct madt_entry {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct madt_ioapic { // type 1
    struct madt_entry hdr;
    uint8_t id;
    uint8_t reserved;
    uint32_t address;
    uint32_t gsi_base;
} __attribute__((packed));

struct madt_override { // type 2: Interrupt Source Override
    struct madt_entry hdr;
    uint8_t bus;
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed));

struct madt_lapic_override { // type 5: 64-bit Local APIC Address Override
    struct madt_entry hdr;
    uint16_t reserved;
    uint64_t address;
} __attribute__((packed));

static bool have_pm; // PM1 control port known
static uint16_t pm1a_cnt, pm1b_cnt;
static bool have_s5;
static uint8_t slp_typa, slp_typb;
static uint32_t fadt_flags;
static struct gas reset_reg;
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

// ACPI stores physical addresses; the Limine HHDM maps all physical memory, so
// convert to a usable pointer. (Values already above the HHDM base are treated
// as already-virtual, covering either Limine RSDP revision.)
static void* map_phys(uint64_t addr)
{
    if (addr == 0) {
        return NULL;
    }
    if (addr >= hhdm_offset) {
        return (void*)addr;
    }
    return phys_to_virt(addr);
}

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
static void parse_s5(struct sdt_header* dsdt)
{
    uint8_t* s = (uint8_t*)dsdt + sizeof(struct sdt_header);
    uint32_t len = dsdt->length - sizeof(struct sdt_header);
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
        uint8_t* p = s + i + 5;      // past "_S5_" and PackageOp
        p += ((*p & 0xC0) >> 6) + 2; // skip pkglength + numelements
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
static void parse_madt(struct madt* madt)
{
    madt_lapic_base = madt->lapic_addr;

    uint8_t* base = (uint8_t*)madt;
    uint32_t total = madt->header.length;
    uint32_t off = sizeof(struct madt); // first Interrupt Controller Structure
    while (off + sizeof(struct madt_entry) <= total) {
        struct madt_entry* e = (struct madt_entry*)(base + off);
        if (e->length < sizeof(struct madt_entry) || off + e->length > total) {
            break;
        }
        switch (e->type) {
        case 1: // I/O APIC
            if (!madt_have_ioapic) {
                struct madt_ioapic* io = (struct madt_ioapic*)e;
                madt_ioapic_base = io->address;
                madt_ioapic_gsi_base = io->gsi_base;
                madt_have_ioapic = true;
            }
            break;
        case 2: // Interrupt Source Override
            if (madt_noverrides < MAX_OVERRIDES) {
                struct madt_override* ov = (struct madt_override*)e;
                madt_overrides[madt_noverrides].source = ov->source;
                madt_overrides[madt_noverrides].gsi = ov->gsi;
                madt_overrides[madt_noverrides].flags = ov->flags;
                madt_noverrides++;
            }
            break;
        case 5: { // Local APIC Address Override (64-bit)
            struct madt_lapic_override* lo = (struct madt_lapic_override*)e;
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

void acpi_init(uint64_t rsdp_addr)
{
    struct rsdp* r = map_phys(rsdp_addr);
    if (r == NULL || !sig_is(r->sig, "RSD PTR ", 8)) {
        return;
    }
    bool use_xsdt = r->revision >= 2 && r->xsdt_addr != 0;
    struct sdt_header* root = map_phys(use_xsdt ? r->xsdt_addr : r->rsdt_addr);
    if (root == NULL) {
        return;
    }

    uint32_t stride = use_xsdt ? 8 : 4;
    uint32_t n = (root->length - (uint32_t)sizeof(struct sdt_header)) / stride;
    uint8_t* entries = (uint8_t*)root + sizeof(struct sdt_header);
    struct sdt_header* fadt = NULL;
    struct sdt_header* madt = NULL;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t ptr = 0;
        memcpy(&ptr, entries + (uint64_t)i * stride, stride);
        struct sdt_header* t = map_phys(ptr);
        if (t == NULL) {
            continue;
        }
        if (sig_is(t->sig, "FACP", 4)) {
            fadt = t;
        } else if (sig_is(t->sig, "APIC", 4)) {
            madt = t;
        }
    }
    if (madt != NULL) {
        parse_madt((struct madt*)madt);
    }
    if (fadt == NULL) {
        return;
    }

    struct fadt* f = (struct fadt*)fadt;
    pm1a_cnt = (uint16_t)f->pm1a_cnt_blk;
    pm1b_cnt = (uint16_t)f->pm1b_cnt_blk;
    fadt_flags = f->flags;
    reset_reg = f->reset_reg;
    reset_value = f->reset_value;
    have_pm = pm1a_cnt != 0;

    // ACPI PM timer: PM_TMR_BLK is an I/O port; the counter is 32-bit when FADT
    // flags bit 8 (TMR_VAL_EXT) is set. Prefer the extended X_PM_TMR_BLK GAS
    // when the FADT is long enough to include it and it is I/O-mapped.
    pm_timer_port = (uint16_t)f->pm_tmr_blk;
    pm_timer_32bit = (fadt_flags & (1u << 8)) != 0;
    if (f->header.length >=
        offsetof(struct fadt, x_pm_tmr_blk) + sizeof(struct gas)) {
        struct gas g = f->x_pm_tmr_blk;
        if (g.address_space == 1 && g.address != 0) {
            pm_timer_port = (uint16_t)g.address;
        }
    }

    uint64_t dsdt_phys = 0;
    if (f->header.length >= offsetof(struct fadt, x_dsdt) + sizeof(uint64_t)) {
        dsdt_phys = f->x_dsdt;
    }
    if (dsdt_phys == 0) {
        dsdt_phys = f->dsdt;
    }
    struct sdt_header* dsdt = map_phys(dsdt_phys);
    if (dsdt != NULL && sig_is(dsdt->sig, "DSDT", 4)) {
        parse_s5(dsdt);
    }
}

#define SLP_EN (1u << 13)

void acpi_shutdown(void)
{
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
        if (reset_reg.address_space == 1) {
            outb((uint16_t)reset_reg.address, reset_value);
        } else if (reset_reg.address_space == 0) {
            *(volatile uint8_t*)map_phys(reset_reg.address) = reset_value;
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
