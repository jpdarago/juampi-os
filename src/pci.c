#include <pci.h>
#include <ports.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

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

pci_addr pci_find(uint16_t vendor, uint16_t device)
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
                    return (pci_addr){(uint8_t)bus, (uint8_t)dev, (uint8_t)func,
                                      true};
                }
            }
        }
    }
    return (pci_addr){0, 0, 0, false};
}

uint32_t pci_bar(pci_addr a, int n)
{
    uint32_t bar = pci_read32(a.bus, a.dev, a.func, (uint8_t)(0x10 + 4 * n));
    // Bit 0 selects the space: 1 = I/O (clear low 2), 0 = memory (clear low 4).
    return (bar & 1) ? (bar & ~0x3u) : (bar & ~0xFu);
}

void pci_enable_bus_master(pci_addr a)
{
    uint32_t cmd = pci_read32(a.bus, a.dev, a.func, 0x04);
    cmd |= (1u << 2) | (1u << 1); // bus master + memory-space decode
    pci_write32(a.bus, a.dev, a.func, 0x04, cmd);
}
