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
// Formatted output — the same specifiers as the vendored printf (%d/%u/%x/%s/
// %c/%p, width/precision, length modifiers). The whole line is emitted under
// the console lock so it stays intact across cores. The format attribute makes
// the compiler type-check the arguments against the format string (-Wformat).
void console_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Write exactly `n` bytes (may contain any byte); used for full-screen frames.
void console_write(const char* s, size_t n);
// Character-cell dimensions of the framebuffer terminal (80x25 without an fb).
void console_dimensions(size_t* cols, size_t* rows);
// Clear the screen and home the cursor.
void console_clear(void);
// Blocking read of one input byte (PS/2 keyboard or serial). Used by the
// shell's line editor; arrow/navigation keys arrive as VT100 escape sequences.
int console_getch(void);

// Mirror every emitted character to `fn(ctx, c)` as well as the framebuffer +
// serial. The windowed desktop shell (src/term.c) sets this so shell output is
// captured into a terminal scrollback; `ctx` is passed back to the sink (the
// terminal instance). Pass (NULL, NULL) to detach. BSP-only.
void console_set_sink(void (*fn)(void*, char), void* ctx);

#endif
