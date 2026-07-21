#ifndef __PCI_H
#define __PCI_H

#include <stdint.h>
#include <stdbool.h>

// PCI configuration space access via the legacy 0xCF8/0xCFC I/O mechanism
// (mechanism #1). Reads/writes are 32-bit at a dword-aligned offset.
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                 uint32_t value);

// A located PCI function, as returned by pci_find().
typedef struct {
    uint8_t bus;  // bus number (0..255)
    uint8_t dev;  // device number on the bus (0..31)
    uint8_t func; // function number within the device (0..7)
    bool found;   // true if pci_find matched a device; when false, the
                  // bus/dev/func fields are meaningless
} pci_addr;

// Scan configuration space for the first function matching (vendor, device).
pci_addr pci_find(uint16_t vendor, uint16_t device);

// Read base address register `n` (0..5), masked to its base address (memory
// BARs clear the low 4 bits; I/O BARs clear the low 2).
uint32_t pci_bar(pci_addr a, int n);

// Set the bus-master bit in the command register so the device can issue DMA.
void pci_enable_bus_master(pci_addr a);

#endif
