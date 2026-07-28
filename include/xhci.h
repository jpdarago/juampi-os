#ifndef __XHCI_H
#define __XHCI_H

#include <stdint.h>
#include <stdbool.h>

// Minimal xHCI (USB 3.x host controller) driver. Milestone 1 brings the
// controller up — reset, the device-context base array, a command ring and an
// event ring — and proves the transfer machinery by round-tripping a NO-OP
// command; milestone 2 enumerates the attached device to its descriptor.
// Poll-driven and BSP-only, like the other early drivers.
void xhci_init(void);
bool xhci_present(void);

// Number of root-hub ports the controller reports (0 if absent).
uint32_t xhci_ports(void);

#endif
