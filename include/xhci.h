#ifndef __XHCI_H
#define __XHCI_H

#include <stdint.h>
#include <stdbool.h>
#include <blockdev.h>

// xHCI (USB 3.x host controller) driver: controller bring-up, enumeration of
// every root port (recursing through hubs), USB mass storage as a blockdev,
// and MSI-X-woken event waits. BSP-only, like the other drivers.
void xhci_init(void);
bool xhci_present(void);

// Number of root-hub ports the controller reports (0 if absent).
uint32_t xhci_ports(void);

// The devices enumerated during init. xhci_device_info fills the identity of
// device index `i` (i < count); returns false past the end.
uint32_t xhci_device_count(void);
bool xhci_device_info(uint32_t i, uint16_t* vid, uint16_t* pid,
                      uint8_t* usb_class);

// Whether the enumerated device is a configured Bulk-Only-Transport
// mass-storage device, its capacity, and its namespace as a block device
// (blockdev.h) for ext2_mount. sectors() is 0 unless the logical block is 512.
bool xhci_msc_ready(void);
uint64_t xhci_msc_blocks(void);
uint32_t xhci_msc_block_size(void);
const struct blockdev* xhci_msc_blockdev(void);

// Which init/enumeration step failed, or NULL (no controller, or all fine).
// Surfaced in the boot report so a real-hardware failure isn't silent.
const char* xhci_fail_reason(void);

// Whether event-ring waits are woken by MSI-X interrupts (false: the polled
// fallback), and the count of interrupts taken (0 in polled mode).
bool xhci_irq_driven(void);
uint64_t xhci_irq_count(void);

// Non-blocking event drain for the idle loops: handles pending HID input
// reports (keystrokes land in the keyboard ring) and re-arms their endpoints.
void xhci_poll(void);

// Whether a USB HID boot-protocol keyboard/mouse is live, and how many input
// reports each has delivered (test/diagnostic counters).
bool xhci_kbd_present(void);
uint64_t xhci_kbd_reports(void);
bool xhci_mouse_present(void);
uint64_t xhci_mouse_reports(void);

#endif
