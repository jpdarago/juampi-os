#ifndef __KEYBOARD_H
#define __KEYBOARD_H

// PS/2 (i8042) keyboard driver: IRQ 1 decodes scancodes into ASCII and fills a
// ring buffer. QEMU attaches one to its display window, and real x86 hardware
// keeps the interface alive through firmware emulation.
void keyboard_init(void);
// Next decoded character, or -1 if none is pending.
int keyboard_poll(void);

#endif
