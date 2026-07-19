#ifndef __PCI_H
#define __PCI_H

#include <stdint.h>

// PCI configuration space access via the legacy 0xCF8/0xCFC I/O mechanism
// (mechanism #1). Reads/writes are 32-bit at a dword-aligned offset.
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                 uint32_t value);

#endif
