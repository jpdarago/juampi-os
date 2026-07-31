#ifndef __MOUSE_H
#define __MOUSE_H

#include <stdint.h>
#include <stdbool.h>

// PS/2 (i8042 aux port) mouse driver: IRQ 12 assembles 3-byte movement packets.
// Enabled at boot; harmless if no mouse is present (init times out cleanly and
// mouse_poll then reports no device). QEMU attaches one to its display window.
void mouse_init(void);

// Drain the movement accumulated since the last call into *dx/*dy (screen
// pixels, +x right / +y down) and report the current button bitmask (bit0 left,
// bit1 right, bit2 middle). Returns true if a mouse was detected at init.
bool mouse_poll(int* dx, int* dy, uint8_t* buttons);

// Feed movement/buttons from another input source (the USB HID mouse) into the
// same accumulators the PS/2 IRQ fills; marks the mouse present.
void mouse_inject(int dx, int dy, uint8_t buttons);

// Parse one aux-port byte into the packet stream. For the keyboard IRQ handler:
// the i8042 output buffer is shared, so when IRQ 1 finds an aux byte pending it
// must route it here rather than treat it as a scancode.
void mouse_handle_byte(uint8_t b);

// Reset packet-assembly phase and drop accumulated movement/buttons — recovers
// a (theoretically) desynced stream, e.g. after a fullscreen program exits.
void mouse_flush(void);

#endif
