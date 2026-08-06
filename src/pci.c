#include <pci.h>
#include <ports.h>
#include <paging.h> // iomap for the MSI-X vector table
#include <apic.h>   // lapic_id for the MSI-X message address

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

// MSI-X: the capability id, message-control bits, and the LAPIC message
// address (physical destination, fixed delivery). The device raises an
// interrupt by writing the address in its vector table with the vector as
// data.
#define PCI_CAP_MSIX 0x11
#define MSIX_MC_ENABLE (1u << 15)    // message control: MSI-X enable
#define MSIX_MC_FUNC_MASK (1u << 14) // message control: mask all vectors
#define MSI_ADDR_BASE 0xFEE00000u

static uint32_t config_address(uint8_t bus, uint8_t dev, uint8_t func,
                               uint8_t offset)
{
    return 0x80000000u | ((uint32_t)bus << 16) |
           ((uint32_t)(dev & 0x1F) << 11) | ((uint32_t)(func & 0x07) << 8) |
           (offset & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, config_address(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                 uint32_t value)
{
    outl(PCI_CONFIG_ADDRESS, config_address(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

struct pci_addr pci_find(uint16_t vendor, uint16_t device)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev,
                                         (uint8_t)func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0) {
                        break; // nothing at this slot at all
                    }
                    continue;
                }
                if ((id & 0xFFFF) == vendor && (id >> 16) == device) {
                    return (struct pci_addr){(uint8_t)bus, (uint8_t)dev,
                                             (uint8_t)func, true};
                }
            }
        }
    }
    return (struct pci_addr){0, 0, 0, false};
}

struct pci_addr pci_find_class(uint8_t class_code, uint8_t subclass,
                               uint8_t prog_if)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev,
                                         (uint8_t)func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0) {
                        break; // nothing at this slot at all
                    }
                    continue;
                }
                // Class/subclass/prog-IF live in the top 3 bytes of dword 0x08.
                uint32_t cls = pci_read32((uint8_t)bus, (uint8_t)dev,
                                          (uint8_t)func, 0x08);
                if (((cls >> 24) & 0xFF) == class_code &&
                    ((cls >> 16) & 0xFF) == subclass &&
                    ((cls >> 8) & 0xFF) == prog_if) {
                    return (struct pci_addr){(uint8_t)bus, (uint8_t)dev,
                                             (uint8_t)func, true};
                }
            }
        }
    }
    return (struct pci_addr){0, 0, 0, false};
}

uint32_t pci_bar(struct pci_addr a, int n)
{
    uint32_t bar = pci_read32(a.bus, a.dev, a.func, (uint8_t)(0x10 + 4 * n));
    // Bit 0 selects the space: 1 = I/O (clear low 2), 0 = memory (clear low 4).
    return (bar & 1) ? (bar & ~0x3u) : (bar & ~0xFu);
}

uint64_t pci_bar64(struct pci_addr a, int n)
{
    uint32_t lo = pci_read32(a.bus, a.dev, a.func, (uint8_t)(0x10 + 4 * n));
    uint32_t hi =
            pci_read32(a.bus, a.dev, a.func, (uint8_t)(0x10 + 4 * (n + 1)));
    return (((uint64_t)hi << 32) | (lo & ~0xFu));
}

void pci_enable_bus_master(struct pci_addr a)
{
    uint32_t cmd = pci_read32(a.bus, a.dev, a.func, 0x04);
    cmd |= (1u << 2) | (1u << 1); // bus master + memory-space decode
    pci_write32(a.bus, a.dev, a.func, 0x04, cmd);
}

uint8_t pci_find_capability(struct pci_addr a, uint8_t cap_id)
{
    // Status register bit 4 says a capability list is present.
    uint32_t status = pci_read32(a.bus, a.dev, a.func, 0x04) >> 16;
    if (!(status & (1u << 4))) {
        return 0;
    }
    // Walk the singly-linked list from the capabilities pointer (offset 0x34).
    uint8_t ptr = pci_read32(a.bus, a.dev, a.func, 0x34) & 0xFF;
    for (int guard = 0; ptr != 0 && guard < 48; guard++) {
        uint32_t cap = pci_read32(a.bus, a.dev, a.func, ptr & 0xFC);
        if ((cap & 0xFF) == cap_id) {
            return ptr;
        }
        ptr = (cap >> 8) & 0xFF; // next-capability pointer
    }
    return 0;
}

bool pci_msix_setup(struct pci_addr a, uint8_t vector,
                    volatile uint32_t** table_out)
{
    uint8_t cap = pci_find_capability(a, PCI_CAP_MSIX);
    if (cap == 0) {
        return false;
    }
    // Table Offset/BIR: which BAR holds the vector table + the offset into it.
    uint32_t tbl = pci_read32(a.bus, a.dev, a.func, (uint8_t)(cap + 4));
    uint32_t bir = tbl & 0x7;
    uint32_t off = tbl & ~0x7u;
    uint64_t bar = (bir == 0) ? pci_bar64(a, 0) : pci_bar(a, (int)bir);
    volatile uint32_t* table =
            iomap(bar + off, PAGE_SZ, PAGEF_P | PAGEF_RW | PAGEF_UC);

    // Entry 0 -> (this core's LAPIC message address, `vector`), unmasked.
    table[0] = MSI_ADDR_BASE | (lapic_id() << 12); // message address low
    table[1] = 0;                                  // message address high
    table[2] = vector;                             // message data
    table[3] = 0;                                  // vector control: unmasked

    // Hand back the mapped table so a driver can later re-mask or re-target the
    // entry (e.g. to poll a completion on a core the vector doesn't point at).
    if (table_out != NULL) {
        *table_out = table;
    }

    // Enable MSI-X and clear the global function mask (message control is the
    // high half of the capability's first dword).
    uint32_t mc = pci_read32(a.bus, a.dev, a.func, cap);
    mc &= ~((uint32_t)MSIX_MC_FUNC_MASK << 16);
    mc |= (uint32_t)MSIX_MC_ENABLE << 16;
    pci_write32(a.bus, a.dev, a.func, cap, mc);
    return true;
}

void pci_msix_mask(volatile uint32_t* table, unsigned entry, bool masked)
{
    // Vector control is dword 3 of each 16-byte entry; bit 0 is the mask bit.
    // When set, the function withholds that message (and records it pending)
    // instead of delivering the interrupt.
    table[entry * 4 + 3] = masked ? 1u : 0u;
}
