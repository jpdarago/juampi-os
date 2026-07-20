#ifndef __CONSOLE_H
#define __CONSOLE_H

#include <limine.h>

#include <stdint.h>
#include <stddef.h>

// The kernel console: output is mirrored to the framebuffer terminal (flanterm
// over the Limine framebuffer, visible in the QEMU window / on the monitor) and
// to the COM1 serial port (headless use and the CI boot test). Before
// console_init — or if there is no framebuffer — output degrades to serial
// only, so early boot and panics always land somewhere.
void console_init(struct limine_framebuffer* fb);
// Re-create the terminal at a new 32bpp framebuffer geometry (after a runtime
// resolution change, gfx_set_mode). `fb` is the linear framebuffer base.
void console_reinit(void* fb, uint64_t w, uint64_t h, uint64_t pitch);
void console_putc(char c);
void console_print(const char* s);
void console_dec(uint64_t v);
void console_hex(uint64_t v);

// Blocking line input with echo and basic editing (backspace), reading from
// whichever input source has a byte first (PS/2 keyboard or serial). Returns
// the line length.
size_t console_read_line(char* buf, size_t max);

#endif
