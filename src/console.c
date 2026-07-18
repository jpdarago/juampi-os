#include <console.h>
#include <serial.h>
#include <keyboard.h>

#include "flanterm/flanterm.h"
#include "flanterm/flanterm_backends/fb.h"

// The single flanterm instance, backed by its internal static bump pool (NULL
// malloc/free below), so the console needs no kernel heap and can come up
// before the memory subsystem.
static struct flanterm_context* ft;

void console_init(struct limine_framebuffer* fb)
{
    if (fb == NULL || fb->memory_model != LIMINE_FRAMEBUFFER_RGB) {
        return; // headless: console stays serial-only
    }
    ft = flanterm_fb_init(NULL, NULL, fb->address, fb->width, fb->height,
                          fb->pitch, fb->red_mask_size, fb->red_mask_shift,
                          fb->green_mask_size, fb->green_mask_shift,
                          fb->blue_mask_size, fb->blue_mask_shift, NULL, NULL,
                          NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0,
                          FLANTERM_FB_ROTATE_0);
}

// Both sinks are real terminals: '\n' is a pure line feed, so newlines are
// expanded to CRLF for each.
void console_putc(char c)
{
    if (c == '\n') {
        serial_putc('\r');
        if (ft) {
            flanterm_write(ft, "\r", 1);
        }
    }
    serial_putc(c);
    if (ft) {
        flanterm_write(ft, &c, 1);
    }
}

void console_print(const char* s)
{
    while (*s) {
        console_putc(*s++);
    }
}

void console_dec(uint64_t v)
{
    char buf[21];
    int i = 20;
    buf[i--] = '\0';
    if (v == 0) {
        buf[i--] = '0';
    }
    while (v > 0) {
        buf[i--] = '0' + (v % 10);
        v /= 10;
    }
    console_print(&buf[i + 1]);
}

void console_hex(uint64_t v)
{
    console_print("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        console_putc("0123456789abcdef"[(v >> shift) & 0xF]);
    }
}

// Blocking read from whichever input source has a byte first: the PS/2
// keyboard ring buffer (fed by IRQ 1) or the serial FIFO. hlt naps until the
// next interrupt (timer, keyboard) rather than spinning hot.
static char console_getc(void)
{
    for (;;) {
        int c = keyboard_poll();
        if (c >= 0) {
            return (char)c;
        }
        c = serial_poll();
        if (c >= 0) {
            return (char)c;
        }
        __asm__ __volatile__("hlt");
    }
}

size_t console_read_line(char* buf, size_t max)
{
    size_t n = 0;
    for (;;) {
        char c = console_getc();
        if (c == '\r' || c == '\n') {
            console_print("\n");
            buf[n] = '\0';
            return n;
        }
        if (c == 0x7F || c == 0x08) { // DEL / backspace
            if (n > 0) {
                n--;
                console_print("\b \b"); // erase on the terminal
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F && n < max - 1) {
            buf[n++] = c;
            console_putc(c); // echo
        }
    }
}
