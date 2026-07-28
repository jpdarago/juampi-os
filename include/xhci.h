#ifndef __XHCI_H
#define __XHCI_H

#include <stdint.h>
#include <stdbool.h>
#include <blockdev.h>

// Minimal xHCI (USB 3.x host controller) driver. Milestone 1 brings the
// controller up — reset, the device-context base array, a command ring and an
// event ring — proves the transfer machinery with a NO-OP command, then resets
// the connected port, addresses the device, and reads its device descriptor.
// Poll-driven and BSP-only, like the other early drivers.
void xhci_init(void);
bool xhci_present(void);

// Number of root-hub ports the controller reports (0 if absent).
uint32_t xhci_ports(void);

// The device enumerated during init (milestone 1 stops at the descriptor):
// whether one was addressed, and its USB vendor/product id and device class.
bool xhci_device_found(void);
uint16_t xhci_vid(void);
uint16_t xhci_pid(void);
uint8_t xhci_class(void);

// Whether the enumerated device is a configured Bulk-Only-Transport
// mass-storage device, its capacity, and its namespace as a block device
// (blockdev.h) for ext2_mount. sectors() is 0 unless the logical block is 512.
bool xhci_msc_ready(void);
uint64_t xhci_msc_blocks(void);
uint32_t xhci_msc_block_size(void);
const blockdev* xhci_msc_blockdev(void);

// Which init/enumeration step failed, or NULL (no controller, or all fine).
// Surfaced in the boot report so a real-hardware failure isn't silent.
const char* xhci_fail_reason(void);

// Whether event-ring waits are woken by MSI-X interrupts (false: the polled
// fallback), and the count of interrupts taken (0 in polled mode).
bool xhci_irq_driven(void);
uint64_t xhci_irq_count(void);

#endif
