#include <shell.h>
#include <console.h>
#include <luashell.h>
#include <fault.h>
#include <ksym.h>
#include <gfx.h>
#include <qoi.h>
#include <kmodule.h>
#include <memory.h>
#include <highlight.h>

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

// Word character for word-wise motion/kills: identifier chars (letters, digits,
// underscore). Everything else (spaces, operators, brackets) is a separator.
static bool is_word(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// Move one word left/right from `cur`: skip separators, then skip word chars —
// the usual readline/emacs behavior.
static size_t word_left(const char* buf, size_t cur)
{
    while (cur > 0 && !is_word(buf[cur - 1])) {
        cur--;
    }
    while (cur > 0 && is_word(buf[cur - 1])) {
        cur--;
    }
    return cur;
}

static size_t word_right(const char* buf, size_t n, size_t cur)
{
    while (cur < n && !is_word(buf[cur])) {
        cur++;
    }
    while (cur < n && is_word(buf[cur])) {
        cur++;
    }
    return cur;
}

// Redraw the current line in place: return to column 0, reprint the prompt and
// the syntax-highlighted buffer, erase anything left from a longer previous
// line, then move the cursor back to its logical column. Cursor motion counts
// display columns, so the SGR escapes in the colorized text don't skew it (as
// byte-based \b arithmetic would). The scratch buffer holds the worst-case
// expansion; highlight_lua() falls back to raw text if it ever overflows.
static void redraw(const char* prompt, const char* buf, size_t n, size_t cur)
{
    static char colored[LINE_MAX * 12];
    highlight_lua(buf, n, colored, sizeof colored);
    console_print("\r");
    console_print(prompt);
    console_print(colored);
    console_print("\033[0m\033[K");
    // The cursor is now at end-of-line (column promptw + n); step it left to
    // promptw + cur. Nothing to do when the cursor is already at the end.
    if (n > cur) {
        console_print("\033[");
        console_dec(n - cur);
        console_print("D");
    }
}

// Replace the edit buffer with `src`, place the cursor at the end, and redraw
// (used on history recall).
static void set_line(const char* prompt, char* buf, size_t* n, size_t* cur,
                     const char* src)
{
    size_t i = 0;
    for (; src[i] != '\0' && i < LINE_MAX - 1; i++) {
        buf[i] = src[i];
    }
    buf[i] = '\0';
    *n = i;
    *cur = i;
    redraw(prompt, buf, *n, *cur);
}

// Read the tail of an ESC '[' / ESC 'O' control sequence: collect the parameter
// and intermediate bytes into `params` (NUL-terminated) and return the final
// byte via `final_out`. Returns false only if the input stream ends.
static bool read_seq(char* params, size_t cap, char* final_out)
{
    size_t k = 0;
    for (;;) {
        int ch = console_getch();
        if (ch < 0) {
            return false;
        }
        if (ch >= 0x40 && ch <= 0x7E) { // final byte
            params[k] = '\0';
            *final_out = (char)ch;
            return true;
        }
        if (k + 1 < cap) {
            params[k++] = (char)ch;
        }
    }
}

// Read a line with full in-line editing: insert/delete at the cursor,
// left/right and Home/End, word motion (Ctrl-/Alt-arrows, Alt-b/f), the usual
// Ctrl-A/E/U/ K/W kills, and up/down history recall. `n` is the line length,
// `cur` the cursor position in [0, n]; every mutation re-highlights via
// redraw().
static void shell_read_line(const char* prompt, char* buf, size_t max)
{
    console_print(prompt);
    size_t n = 0;   // line length
    size_t cur = 0; // cursor position within [0, n]
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
        if (c == 0x01) { // Ctrl-A: home
            cur = 0;
            redraw(prompt, buf, n, cur);
            continue;
        }
        if (c == 0x05) { // Ctrl-E: end
            cur = n;
            redraw(prompt, buf, n, cur);
            continue;
        }
        if (c == 0x02) { // Ctrl-B: left
            if (cur > 0) {
                cur--;
                redraw(prompt, buf, n, cur);
            }
            continue;
        }
        if (c == 0x06) { // Ctrl-F: right
            if (cur < n) {
                cur++;
                redraw(prompt, buf, n, cur);
            }
            continue;
        }
        if (c == 0x15) { // Ctrl-U: kill to start of line
            if (cur > 0) {
                size_t rest = n - cur;
                for (size_t i = 0; i < rest; i++) {
                    buf[i] = buf[cur + i];
                }
                n = rest;
                cur = 0;
                buf[n] = '\0';
                redraw(prompt, buf, n, cur);
            }
            continue;
        }
        if (c == 0x0B) { // Ctrl-K: kill to end of line
            if (cur < n) {
                n = cur;
                buf[n] = '\0';
                redraw(prompt, buf, n, cur);
            }
            continue;
        }
        if (c == 0x17) { // Ctrl-W: kill the word before the cursor
            if (cur > 0) {
                size_t start = word_left(buf, cur);
                size_t rest = n - cur;
                for (size_t i = 0; i < rest; i++) {
                    buf[start + i] = buf[cur + i];
                }
                n = start + rest;
                cur = start;
                buf[n] = '\0';
                redraw(prompt, buf, n, cur);
            }
            continue;
        }
        if (c == 0x7F ||
            c == 0x08) { // Backspace: delete char before the cursor
            if (cur > 0) {
                for (size_t i = cur - 1; i + 1 < n; i++) {
                    buf[i] = buf[i + 1];
                }
                n--;
                cur--;
                buf[n] = '\0';
                redraw(prompt, buf, n, cur);
            }
            continue;
        }
        if (c == 27) { // ESC — a VT100/ANSI control sequence
            int c2 = console_getch();
            if (c2 == 'b') { // Alt-b: word left
                cur = word_left(buf, cur);
                redraw(prompt, buf, n, cur);
                continue;
            }
            if (c2 == 'f') { // Alt-f: word right
                cur = word_right(buf, n, cur);
                redraw(prompt, buf, n, cur);
                continue;
            }
            if (c2 != '[' && c2 != 'O') {
                continue;
            }
            char params[8];
            char fin;
            if (!read_seq(params, sizeof params, &fin)) {
                continue;
            }
            // A ";5" (Ctrl) or ";3" (Alt) parameter turns an arrow into word
            // motion, e.g. Ctrl-Right arrives as ESC [ 1 ; 5 C.
            bool word = str_eq(params, "1;5") || str_eq(params, "1;3");
            switch (fin) {
            case 'A': // up: older history
                if (hist_count > 0) {
                    if (browse < 0) {
                        browse = 0;
                    } else if (browse + 1 < hist_count) {
                        browse++;
                    }
                    int idx =
                            (hist_next - 1 - browse + 2 * HIST_MAX) % HIST_MAX;
                    set_line(prompt, buf, &n, &cur, history[idx]);
                }
                break;
            case 'B': // down: newer history
                if (browse >= 0) {
                    browse--;
                    if (browse < 0) {
                        set_line(prompt, buf, &n, &cur, "");
                    } else {
                        int idx = (hist_next - 1 - browse + 2 * HIST_MAX) %
                                  HIST_MAX;
                        set_line(prompt, buf, &n, &cur, history[idx]);
                    }
                }
                break;
            case 'C': // right (a word at a time when Ctrl/Alt-modified)
                cur = word ? word_right(buf, n, cur)
                           : (cur < n ? cur + 1 : cur);
                redraw(prompt, buf, n, cur);
                break;
            case 'D': // left
                cur = word ? word_left(buf, cur) : (cur > 0 ? cur - 1 : cur);
                redraw(prompt, buf, n, cur);
                break;
            case 'H': // home
                cur = 0;
                redraw(prompt, buf, n, cur);
                break;
            case 'F': // end
                cur = n;
                redraw(prompt, buf, n, cur);
                break;
            case '~': // numbered nav: 1/7 home, 4/8 end, 3 delete-at-cursor
                if (str_eq(params, "1") || str_eq(params, "7")) {
                    cur = 0;
                    redraw(prompt, buf, n, cur);
                } else if (str_eq(params, "4") || str_eq(params, "8")) {
                    cur = n;
                    redraw(prompt, buf, n, cur);
                } else if (str_eq(params, "3")) {
                    if (cur < n) {
                        for (size_t i = cur; i + 1 < n; i++) {
                            buf[i] = buf[i + 1];
                        }
                        n--;
                        buf[n] = '\0';
                        redraw(prompt, buf, n, cur);
                    }
                }
                break;
            default:
                break;
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F &&
            n < max - 1) { // printable: insert at cursor
            for (size_t i = n; i > cur; i--) {
                buf[i] = buf[i - 1];
            }
            buf[cur] = (char)c;
            n++;
            cur++;
            buf[n] = '\0';
            // Re-highlight the whole line: a keystroke can change the color of
            // earlier characters (e.g. finishing a keyword or closing a
            // string).
            redraw(prompt, buf, n, cur);
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
