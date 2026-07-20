#include <acpi.h>
#include <paging.h>
#include <ports.h>
#include <utils.h>

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

static bool have_pm; // PM1 control port known
static uint16_t pm1a_cnt, pm1b_cnt;
static bool have_s5;
static uint8_t slp_typa, slp_typb;
static uint32_t fadt_flags;
static struct gas reset_reg;
static uint8_t reset_value;

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
    for (uint32_t i = 0; i < n; i++) {
        uint64_t ptr = 0;
        memcpy(&ptr, entries + (uint64_t)i * stride, stride);
        struct sdt_header* t = map_phys(ptr);
        if (t != NULL && sig_is(t->sig, "FACP", 4)) {
            fadt = t;
            break;
        }
    }
    if (fadt == NULL) {
        return;
    }

    uint8_t* f = (uint8_t*)fadt;
    uint32_t p1a = 0, p1b = 0;
    memcpy(&p1a, f + 64, 4); // PM1a_CNT_BLK
    memcpy(&p1b, f + 68, 4); // PM1b_CNT_BLK
    pm1a_cnt = (uint16_t)p1a;
    pm1b_cnt = (uint16_t)p1b;
    memcpy(&fadt_flags, f + 112, 4);
    memcpy(&reset_reg, f + 116, sizeof(struct gas));
    reset_value = f[128];
    have_pm = pm1a_cnt != 0;

    uint64_t dsdt_phys = 0;
    if (fadt->length >= 148) {
        memcpy(&dsdt_phys, f + 140, 8); // X_DSDT
    }
    if (dsdt_phys == 0) {
        uint32_t d = 0;
        memcpy(&d, f + 40, 4); // DSDT
        dsdt_phys = d;
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
