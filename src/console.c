#include <console.h>
#include <serial.h>
#include <keyboard.h>
#include <spinlock.h>
#include <memory.h>
#include <net.h>
#include <xhci.h>
#include <gfx.h>
#include <audio.h>

#include <printf/printf.h>

#include <stdarg.h>

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
static struct spinlock console_lock;

// Optional extra sink (the windowed terminal's scrollback). Receives the raw
// character stream — including the SGR escapes from the highlighter — before
// the CRLF expansion the real terminals need, plus an opaque `ctx` so the sink
// can be a per-instance widget rather than a singleton. Set only on the BSP by
// the desktop shell; NULL otherwise.
static void (*extra_sink)(void*, char);
static void* extra_sink_ctx;

void console_set_sink(void (*fn)(void*, char), void* ctx)
{
    // Publish under the lock: emit() reads these while holding it, so an
    // unlocked store could tear against a concurrent print from another core.
    spin_lock(&console_lock);
    extra_sink = fn;
    extra_sink_ctx = ctx;
    spin_unlock(&console_lock);
}

// Write one character to both sinks. Not locked — callers hold console_lock.
// Both sinks are real terminals: '\n' is a pure line feed, so newlines are
// expanded to CRLF for each.
static void emit(char c)
{
    if (extra_sink) {
        extra_sink(extra_sink_ctx, c);
    }
    // flanterm draws straight to the raw framebuffer. When a compositor owns
    // the screen (double-buffering: the windowed desktop / editor), those
    // writes are stale — they used to be clobbered by the full flip every
    // frame, but damage-tracked flipping would leave them as corruption. Skip
    // flanterm then; the desktop terminal grid (extra_sink) is the display.
    bool fbterm = ft != NULL && !gfx_buffered();
    if (c == '\n') {
        serial_putc('\r');
        if (fbterm) {
            flanterm_write(ft, "\r", 1);
        }
    }
    serial_putc(c);
    if (fbterm) {
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

// Per-character output for vfctprintf: feed the console's single writer.
static void printf_emit(char c, void* arg)
{
    (void)arg;
    emit(c);
}

void console_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    spin_lock(&console_lock);
    vfctprintf(printf_emit, NULL, fmt, ap);
    spin_unlock(&console_lock);
    va_end(ap);
}

// Write exactly `n` bytes to both sinks. Unlike console_print this takes a
// length rather than a NUL terminator, so it can emit a whole pre-composed
// frame (e.g. the full-screen editor) that may contain arbitrary bytes in one
// call.
void console_write(const char* s, size_t n)
{
    spin_lock(&console_lock);
    for (size_t i = 0; i < n; i++) {
        emit(s[i]);
    }
    spin_unlock(&console_lock);
}

// Character-cell dimensions of the framebuffer terminal. Falls back to a
// conventional 80x25 when there is no framebuffer (serial-only early boot).
void console_dimensions(size_t* cols, size_t* rows)
{
    if (ft) {
        flanterm_get_dimensions(ft, cols, rows);
    } else {
        *cols = 80;
        *rows = 25;
    }
}

// Clear the screen and home the cursor (ANSI; handled by flanterm and by a
// serial terminal alike).
void console_clear(void)
{
    console_print("\033[2J\033[H");
    // The ANSI clear only repaints the cells flanterm's own cache believes
    // changed, so pixels drawn straight to the framebuffer — the raytracer via
    // fb.canvas(), fb.rect/image, the blitted logo — bypass that cache and
    // would survive the clear as stale color. Force flanterm to repaint every
    // pixel from its background + character grid so the screen is truly wiped.
    if (ft) {
        spin_lock(&console_lock);
        flanterm_full_refresh(ft);
        spin_unlock(&console_lock);
    }
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
        net_poll();   // keep the network stack live while waiting for a key
        xhci_poll();  // pump USB HID input into the keyboard ring
        audio_pump(); // keep the audio mixer fed between keystrokes
        __asm__ __volatile__("hlt");
    }
}
