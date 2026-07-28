#ifndef __KEYBOARD_H
#define __KEYBOARD_H

// PS/2 (i8042) keyboard driver: IRQ 1 decodes scancodes into ASCII and fills a
// ring buffer. QEMU attaches one to its display window, and real x86 hardware
// keeps the interface alive through firmware emulation.
void keyboard_init(void);
// Next decoded character, or -1 if none is pending.
int keyboard_poll(void);
// Feed a character (or an ESC [ <final> arrow sequence) from another input
// source — the USB HID keyboard — into the same ring the PS/2 IRQ fills.
void keyboard_inject(char c);
void keyboard_inject_seq(char final);

#endif
