#include <console.h>
#include <serial.h>
#include <keyboard.h>
#include <spinlock.h>
#include <memory.h>

#include <printf/printf.h>

#include "flanterm/flanterm.h"
#include "flanterm/flanterm_backends/fb.h"

// Output sink for the vendored printf (printf/sprintf write here). Routes a
// single character to the console.
void putchar_(char c)
{
    console_putc(c);
}

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

// Heap-backed allocator for flanterm, so the terminal can be re-created on a
// resolution change (the default bump pool is one-shot).
static void* ft_malloc(size_t n)
{
    return alloc(&heap_default()->base, (ptrdiff_t)n, 16, 1);
}
static void ft_free(void* p, size_t n)
{
    (void)n;
    heap_free(heap_default(), p);
}
static bool ft_heap; // is the current context heap-allocated (deinit-able)?

// Re-point the console at a new 32bpp framebuffer geometry (after
// gfx_set_mode). The channel layout matches gfx_set_mode's DISPI mode (xRGB:
// B0/G8/R16).
void console_reinit(void* fb, uint64_t w, uint64_t h, uint64_t pitch)
{
    // Called from the shell (BSP) on a mode change; no other core prints then.
    if (ft != NULL && ft_heap) {
        flanterm_deinit(ft, ft_free);
    }
    ft = flanterm_fb_init(ft_malloc, ft_free, (uint32_t*)fb, w, h, pitch, 8, 16,
                          8, 8, 8, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                          NULL, 0, 0, 0, 0, 0, 0, FLANTERM_FB_ROTATE_0);
    ft_heap = true;
}

// Serializes console output across cores (the APs share these sinks once SMP is
// up). Taken at the string level so a whole print/number stays intact; the
// unlocked emit() below is the single writer the locked wrappers call.
static spinlock console_lock;

// Write one character to both sinks. Not locked — callers hold console_lock.
// Both sinks are real terminals: '\n' is a pure line feed, so newlines are
// expanded to CRLF for each.
static void emit(char c)
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

void console_putc(char c)
{
    spin_lock(&console_lock);
    emit(c);
    spin_unlock(&console_lock);
}

void console_print(const char* s)
{
    spin_lock(&console_lock);
    while (*s) {
        emit(*s++);
    }
    spin_unlock(&console_lock);
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
    spin_lock(&console_lock);
    emit('0');
    emit('x');
    for (int shift = 60; shift >= 0; shift -= 4) {
        emit("0123456789abcdef"[(v >> shift) & 0xF]);
    }
    spin_unlock(&console_lock);
}

// Clear the screen and home the cursor (ANSI; handled by flanterm and by a
// serial terminal alike).
void console_clear(void)
{
    console_print("\033[2J\033[H");
}

// Blocking read of one byte from whichever input source has one first: the PS/2
// keyboard ring buffer (fed by IRQ 1) or the serial FIFO. hlt naps until the
// next interrupt (timer, keyboard) rather than spinning hot.
int console_getch(void)
{
    for (;;) {
        int c = keyboard_poll();
        if (c >= 0) {
            return c;
        }
        c = serial_poll();
        if (c >= 0) {
            return c;
        }
        __asm__ __volatile__("hlt");
    }
}

size_t console_read_line(char* buf, size_t max)
{
    size_t n = 0;
    for (;;) {
        char c = (char)console_getch();
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
