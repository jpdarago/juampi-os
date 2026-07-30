#ifndef __E1000_H
#define __E1000_H

#include <stdint.h>
#include <stdbool.h>

// Driver for the Intel 82540EM ("e1000") gigabit NIC — the card QEMU emulates
// with `-nic user,model=e1000`, and a real chip whose register interface this
// targets verbatim (see docs/networking.md). RX is interrupt-driven (legacy PCI
// INTx, routed via the ACPI _PRT); the ISR only bumps a counter, and net_poll()
// drains the ring from the main thread (the interrupt wakes the idle hlt).

// Probe PCI for the e1000, map its register BAR, reset it, set up the RX/TX
// descriptor rings and bring the link up. Returns false if no card is present.
bool e1000_init(void);

// True once e1000_init() has found and configured a card.
bool e1000_present(void);

// Copy the card's 6-byte MAC address into `out`.
void e1000_mac(uint8_t out[6]);

// Transmit one fully-built Ethernet frame (CRC excluded — the card appends it).
// Blocks briefly until the card confirms the descriptor was sent. Returns false
// if no card is present or the send was not confirmed.
bool e1000_tx(const void* frame, uint16_t len);

// One received Ethernet frame. `data` points into the driver's RX buffer and
// stays valid only until the next e1000_rx_poll() call.
struct e1000_frame {
    const uint8_t* data;
    uint16_t len;
};

// Fetch the next received frame. Returns true and fills *out when a frame was
// waiting (more may remain — call again in a loop); false once the RX ring is
// drained.
bool e1000_rx_poll(struct e1000_frame* out);

// Enable the receive interrupt: route the card's PCI INTx line (preferring the
// ACPI _PRT, else the firmware Interrupt Line register) and unmask RX causes.
// Needs interrupts + full uACPI up; call after net_init has a live card.
void e1000_enable_irq(void);

// True once the RX interrupt is routed and enabled (else RX is purely polled).
bool e1000_irq_driven(void);
// Count of receive interrupts taken (diagnostic; proves the ISR fired).
uint64_t e1000_irq_count(void);

#endif
