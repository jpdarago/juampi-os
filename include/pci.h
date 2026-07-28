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

// Scan for the first function matching a class code (class/subclass/prog-IF at
// config offset 0x0B/0x0A/0x09). Used for devices with no fixed vendor/device
// id, e.g. an NVMe controller (0x01 / 0x08 / 0x02).
pci_addr pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if);

// Read base address register `n` (0..5), masked to its base address (memory
// BARs clear the low 4 bits; I/O BARs clear the low 2).
uint32_t pci_bar(pci_addr a, int n);

// Read a 64-bit memory BAR as the pair (BAR[n] low, BAR[n+1] high), masked to
// its base address. For the 64-bit BARs NVMe and other modern devices use.
uint64_t pci_bar64(pci_addr a, int n);

// Set the bus-master bit in the command register so the device can issue DMA.
void pci_enable_bus_master(pci_addr a);

// Walk the capability list for the capability with the given id (e.g. 0x11 =
// MSI-X); returns its config-space byte offset, or 0 if absent.
uint8_t pci_find_capability(pci_addr a, uint8_t cap_id);

#endif
