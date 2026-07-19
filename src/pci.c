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
