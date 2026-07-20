#include <shell.h>
#include <console.h>
#include <luashell.h>
#include <fault.h>
#include <ksym.h>
#include <gfx.h>
#include <qoi.h>
#include <kmodule.h>
#include <memory.h>

#include <stddef.h>
#include <stdint.h>

#define LINE_MAX 256

// Boot logo, decoded once from the logo.qoi Limine module and kept resident so
// it can be re-blitted cheaply. The text console (flanterm) keeps its own
// canvas and rewrites the whole framebuffer from it whenever it scrolls, which
// erases anything we drew directly — so the logo has to be redrawn after output
// that may have scrolled, not just once at startup.
static uint32_t* logo_pixels;
static qoi_image logo_img;

static void logo_load(void)
{
    size_t size = 0;
    const void* data = kmodule_find("logo.qoi", &size);
    if (data != NULL) {
        logo_pixels = qoi_decode(&heap_default()->base, data, size, &logo_img);
    }
}

// Blit the logo into the top-right corner (clamped on-screen).
static void logo_draw(void)
{
    if (!gfx_available() || logo_pixels == NULL) {
        return;
    }
    int64_t x = (int64_t)gfx_width() - (int64_t)logo_img.width - 16;
    if (x < 0) {
        x = 0;
    }
    gfx_blit(x, 16, logo_img.width, logo_img.height, logo_pixels);
}

// --- Line editor with command history --------------------------------------
// A ring of recent commands, recalled with the up/down arrows (which arrive as
// VT100 escape sequences from both the serial line and the PS/2 keyboard).

#define HIST_MAX 32
static char history[HIST_MAX][LINE_MAX];
static int hist_count; // number of stored entries (<= HIST_MAX)
static int hist_next;  // ring index of the next slot to write

static bool str_eq(const char* a, const char* b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void hist_add(const char* line)
{
    if (line[0] == '\0') {
        return; // don't store blank lines
    }
    if (hist_count > 0) {
        int last = (hist_next - 1 + HIST_MAX) % HIST_MAX;
        if (str_eq(history[last], line)) {
            return; // collapse immediate duplicates
        }
    }
    size_t i = 0;
    for (; line[i] != '\0' && i < LINE_MAX - 1; i++) {
        history[hist_next][i] = line[i];
    }
    history[hist_next][i] = '\0';
    hist_next = (hist_next + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) {
        hist_count++;
    }
}

// Redraw the current line in place: return to column 0, reprint the prompt and
// buffer, and erase anything left over from a longer previous line.
static void redraw(const char* prompt, const char* buf)
{
    console_print("\r");
    console_print(prompt);
    console_print(buf);
    console_print("\033[K");
}

// Replace the edit buffer with `src` and redraw (used on history recall).
static void set_line(const char* prompt, char* buf, size_t* n, const char* src)
{
    size_t i = 0;
    for (; src[i] != '\0' && i < LINE_MAX - 1; i++) {
        buf[i] = src[i];
    }
    buf[i] = '\0';
    *n = i;
    redraw(prompt, buf);
}

// Read a line with echo, backspace, and up/down history. Cursor stays at the
// end (no left/right editing yet).
static void shell_read_line(const char* prompt, char* buf, size_t max)
{
    console_print(prompt);
    size_t n = 0;
    buf[0] = '\0';
    int browse = -1; // -1 = fresh line; 0 = newest history entry, up to count-1

    for (;;) {
        int c = console_getch();
        if (c == '\r' || c == '\n') {
            console_print("\n");
            buf[n] = '\0';
            hist_add(buf);
            return;
        }
        if (c == 0x7F || c == 0x08) { // backspace
            if (n > 0) {
                buf[--n] = '\0';
                console_print("\b \b");
            }
            continue;
        }
        if (c == 27) { // ESC — a VT100 sequence (arrows/nav)
            if (console_getch() != '[') {
                continue;
            }
            int fin = console_getch();
            if (fin == 'A' && hist_count > 0) { // up: older
                if (browse < 0) {
                    browse = 0;
                } else if (browse + 1 < hist_count) {
                    browse++;
                }
                int idx = (hist_next - 1 - browse + 2 * HIST_MAX) % HIST_MAX;
                set_line(prompt, buf, &n, history[idx]);
            } else if (fin == 'B' && browse >= 0) { // down: newer
                browse--;
                if (browse < 0) {
                    set_line(prompt, buf, &n, "");
                } else {
                    int idx =
                            (hist_next - 1 - browse + 2 * HIST_MAX) % HIST_MAX;
                    set_line(prompt, buf, &n, history[idx]);
                }
            }
            continue; // ignore left/right/home/end for now
        }
        if (c >= 0x20 && c < 0x7F && n < max - 1) {
            buf[n++] = (char)c;
            buf[n] = '\0';
            console_putc((char)c);
        }
    }
}

void shell_run(void)
{
    static char line[LINE_MAX];
    luashell_init();
    logo_load();
    console_print(
            "\njuampiOS Lua shell (Lua 5.4).\n"
            "  math/string/table/coroutine, and 'k' for kernel introspection:\n"
            "  k.cpubrand()  k.freemem()  k.uptime()  k.ncores()  "
            "k.hexdump(addr)\n"
            "  run(name[,arg]) runs a .lua script or a native .elf binary.\n"
            "  bench(fn|name[,arg[,iters]]) -> total,per_call (Lua or "
            "native).\n"
            "  help() lists what's available, dump(t) inspects a table, "
            "clear() clears\n"
            "  the screen, and up/down arrows recall history.\n");
    // Draw the logo after the banner (which may have scrolled the console).
    logo_draw();

    int cont = 0;

    // Recovery point: a CPU fault while evaluating Lua (e.g. a bad k.poke)
    // longjmps here instead of halting. The fault handler entered with
    // interrupts masked and left the interpreter mid-call, so re-enable
    // interrupts and start a fresh interpreter (the old, inconsistent state is
    // abandoned).
    if (setjmp(fault_env) != 0) {
        __asm__ __volatile__("sti");
        console_print("\n[recovered from fault: vector ");
        console_dec(fault_vector);
        console_print(", addr ");
        console_hex(fault_addr);
        console_print(", rip ");
        console_hex(fault_rip);
        uint64_t off = 0;
        const char* fn = ksym_lookup(fault_rip, &off);
        if (fn) {
            console_print(" (");
            console_print(fn);
            console_print(")");
        }
        console_print(" — interpreter reset]\n");
        luashell_init();
        cont = 0;
    }

    for (;;) {
        shell_read_line(cont ? "  >> " : "lua> ", line, sizeof(line));
        fault_armed = true;
        cont = luashell_eval(line);
        fault_armed = false;
        // A completed evaluation may have printed output and scrolled the
        // console, wiping the logo; redraw it so it stays in the corner.
        if (!cont) {
            logo_draw();
        }
    }
}
