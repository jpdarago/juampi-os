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

#endif
