#ifndef __KEYBOARD_H
#define __KEYBOARD_H

// PS/2 (i8042) keyboard driver: IRQ 1 decodes scancodes into ASCII and fills a
// ring buffer. QEMU attaches one to its display window, and real x86 hardware
// keeps the interface alive through firmware emulation.
void keyboard_init(void);
// Next decoded character, or -1 if none is pending.
int keyboard_poll(void);
// Next raw key event for interactive fullscreen apps (games), or -1 if none.
// Returns the PS/2 set-1 make code (bit 7 set for E0-extended keys: arrows,
// right-ctrl) and writes *pressed (1 = key down, 0 = key up). Reports both
// press and release, unlike keyboard_poll(); the two rings are independent.
int keyboard_poll_raw(int* pressed);
// Feed a character (or an ESC [ <final> arrow sequence) from another input
// source — the USB HID keyboard — into the same ring the PS/2 IRQ fills.
void keyboard_inject(char c);
void keyboard_inject_seq(char final);

#endif
