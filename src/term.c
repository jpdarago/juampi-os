// Windowed terminal (see term.h), as a reentrant instance. Two halves:
//
//   * a scrollback grid of coloured cells, appended to by term_write() which is
//     installed as the console sink — so everything the shell prints (results,
//     errors, and the highlighter's SGR escapes) is captured and rendered
//     inside a microui window instead of straight to the framebuffer.
//   * a live input line with the same editing + history as the classic shell
//     (src/shell.c), highlighted as you type via highlight_lua(); Enter echoes
//     the line into the scrollback and feeds it to luashell_eval().
//
// term_build() draws the whole thing into a microui window each frame. All
// state lives in `struct term`, allocated from the caller's arena by
// term_open() — no file-scope mutable statics, so a second terminal is just a
// second term*.

#include <term.h>
#include <console.h>
#include <luashell.h>
#include <fault.h>
#include <highlight.h>
#include <gfx.h>

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "microui/microui.h"

#define TCOLS 220
#define TROWS 1000
#define LINE_MAX 256
#define HIST_MAX 32
#define GW 8
#define GH 16

// Palette indices stored per cell; index 0 is the default foreground.
enum { C_DEF, C_GREEN, C_YELLOW, C_MAGENTA, C_GREY, C_N };
static const uint32_t palette[C_N] = {
        0xd4d4d4, 0x6ac46a, 0xd4c46a, 0xc46ac4, 0x808080,
};
#define TERM_BG 0x0e1116

typedef struct {
    char ch;
    uint8_t color;
} Cell;

typedef Cell
        term_row[TCOLS]; // one scrollback line (the grid is an array of these)

struct term {
    allocator* mem;

    term_row* grid; // TROWS rows, ring-indexed by absolute row
    int wrow;       // absolute index of the line being written (monotonic)
    int wcol;       // write column on that line
    uint8_t wcolor; // current SGR colour index
    int scroll_off; // lines scrolled up from the bottom (0 = follow output)

    // SGR/escape parser state for the output stream.
    int oesc;
    char ocsi[16];
    int ocsi_len;

    // Input line + history.
    char in[LINE_MAX];
    int in_len, in_cur;
    const char* prompt;
    int cont; // continuation (multi-line statement in progress)
    char history[HIST_MAX][LINE_MAX];
    int hist_count, hist_next;
    int hist_browse; // -1 = editing; else steps back from newest
    char hist_saved[LINE_MAX];

    // Input-stream escape parser state (separate from the output one).
    int kesc;
    char kcsi[16];
    int kcsi_len;
};

static Cell* line_at(term* t, int abs_row)
{
    return t->grid[((abs_row % TROWS) + TROWS) % TROWS];
}

static void clear_line(term* t, int abs_row)
{
    Cell* l = line_at(t, abs_row);
    for (int i = 0; i < TCOLS; i++) {
        l[i].ch = ' ';
        l[i].color = C_DEF;
    }
}

term* term_open(allocator* mem)
{
    term* t = new (mem, term, 1);
    t->mem = mem;
    t->grid = new (mem, term_row, TROWS);
    for (int r = 0; r < TROWS; r++) {
        clear_line(t, r);
    }
    t->wcolor = C_DEF;
    t->prompt = "lua> ";
    t->hist_browse = -1;
    return t; // remaining fields zeroed by the allocator contract
}

static void term_newline(term* t)
{
    t->wrow++;
    t->wcol = 0;
    clear_line(t, t->wrow);
}

static void put_cell(term* t, char c)
{
    if (t->wcol >= TCOLS) {
        term_newline(t);
    }
    Cell* l = line_at(t, t->wrow);
    l[t->wcol].ch = c;
    l[t->wcol].color = t->wcolor;
    t->wcol++;
}

// Map an SGR parameter to a palette index (only the codes the highlighter and
// console actually emit; anything else resets to the default).
static uint8_t sgr_to_color(int code)
{
    switch (code) {
    case 32:
        return C_GREEN;
    case 33:
        return C_YELLOW;
    case 35:
        return C_MAGENTA;
    case 90:
        return C_GREY;
    default:
        return C_DEF;
    }
}

static int atoi_n(const char* s, int len)
{
    int v = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            break;
        }
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static void handle_csi(term* t, char final)
{
    t->ocsi[t->ocsi_len] = '\0';
    switch (final) {
    case 'm': // SGR colour (single code is all the shell emits)
        t->wcolor = sgr_to_color(atoi_n(t->ocsi, t->ocsi_len));
        break;
    case 'K': // erase from the cursor to end of line
        for (int c = t->wcol; c < TCOLS; c++) {
            line_at(t, t->wrow)[c].ch = ' ';
            line_at(t, t->wrow)[c].color = C_DEF;
        }
        break;
    case 'J': // clear screen (console_clear sends ESC[2J)
        for (int r = 0; r < TROWS; r++) {
            clear_line(t, r);
        }
        t->wrow = 0;
        t->wcol = 0;
        break;
    case 'H': // cursor home
        t->wcol = 0;
        break;
    default:
        break;
    }
}

void term_write(void* ctx, char c)
{
    term* t = (term*)ctx;
    if (t->oesc == 1) {
        t->oesc = (c == '[') ? 2 : 0;
        t->ocsi_len = 0;
        return;
    }
    if (t->oesc == 2) {
        if (c >= 0x40 && c <= 0x7e) {
            handle_csi(t, c);
            t->oesc = 0;
        } else if (t->ocsi_len < (int)sizeof(t->ocsi) - 1) {
            t->ocsi[t->ocsi_len++] = c;
        }
        return;
    }
    switch (c) {
    case 27:
        t->oesc = 1;
        return;
    case '\n':
        term_newline(t);
        break;
    case '\r':
        t->wcol = 0;
        break;
    case '\b':
        if (t->wcol > 0) {
            t->wcol--;
        }
        break;
    case '\t': {
        int stop = (t->wcol + 8) & ~7;
        while (t->wcol < stop && t->wcol < TCOLS) {
            put_cell(t, ' ');
        }
        break;
    }
    default:
        if ((unsigned char)c >= 32) {
            put_cell(t, c);
        }
        break;
    }
    t->scroll_off = 0; // any output snaps the view back to the bottom
}

// --- input line + history ---------------------------------------------------

static int slen(const char* s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static bool is_word(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}
static int word_left(term* t, int cur)
{
    while (cur > 0 && !is_word(t->in[cur - 1])) {
        cur--;
    }
    while (cur > 0 && is_word(t->in[cur - 1])) {
        cur--;
    }
    return cur;
}
static int word_right(term* t, int cur)
{
    while (cur < t->in_len && !is_word(t->in[cur])) {
        cur++;
    }
    while (cur < t->in_len && is_word(t->in[cur])) {
        cur++;
    }
    return cur;
}

static void hist_add(term* t, const char* line)
{
    if (line[0] == '\0') {
        return;
    }
    if (t->hist_count > 0) {
        int last = (t->hist_next - 1 + HIST_MAX) % HIST_MAX;
        int i = 0;
        while (t->history[last][i] && t->history[last][i] == line[i]) {
            i++;
        }
        if (t->history[last][i] == line[i]) {
            return; // identical to the previous entry
        }
    }
    int i = 0;
    for (; line[i] && i < LINE_MAX - 1; i++) {
        t->history[t->hist_next][i] = line[i];
    }
    t->history[t->hist_next][i] = '\0';
    t->hist_next = (t->hist_next + 1) % HIST_MAX;
    if (t->hist_count < HIST_MAX) {
        t->hist_count++;
    }
}

static void load_line(term* t, const char* s)
{
    int i = 0;
    for (; s[i] && i < LINE_MAX - 1; i++) {
        t->in[i] = s[i];
    }
    t->in_len = i;
    t->in_cur = i;
    t->in[t->in_len] = '\0';
}

static void hist_prev(term* t)
{
    if (t->hist_count == 0) {
        return;
    }
    if (t->hist_browse == -1) {
        t->in[t->in_len] = '\0';
        int j = 0;
        for (; t->in[j]; j++) {
            t->hist_saved[j] = t->in[j];
        }
        t->hist_saved[j] = '\0';
        t->hist_browse = 0;
    } else if (t->hist_browse < t->hist_count - 1) {
        t->hist_browse++;
    }
    int idx = (t->hist_next - 1 - t->hist_browse + 2 * HIST_MAX) % HIST_MAX;
    load_line(t, t->history[idx]);
}

static void hist_next_(term* t)
{
    if (t->hist_browse == -1) {
        return;
    }
    if (t->hist_browse > 0) {
        t->hist_browse--;
        int idx = (t->hist_next - 1 - t->hist_browse + 2 * HIST_MAX) % HIST_MAX;
        load_line(t, t->history[idx]);
    } else {
        t->hist_browse = -1;
        load_line(t, t->hist_saved);
    }
}

static void submit(term* t)
{
    t->in[t->in_len] = '\0';
    // Echo the prompt + highlighted line into the scrollback (through the
    // console, so it is also mirrored to serial), then evaluate.
    char hl[LINE_MAX * 8];
    console_print(t->prompt);
    highlight_lua(t->in, (size_t)t->in_len, hl, sizeof hl);
    console_print(hl);
    console_print("\033[0m\n");

    hist_add(t, t->in);
    t->hist_browse = -1;

    fault_armed = true;
    t->cont = luashell_eval(t->in);
    fault_armed = false;

    t->prompt = t->cont ? "  >> " : "lua> ";
    t->in_len = 0;
    t->in_cur = 0;
    t->in[0] = '\0';
    t->scroll_off = 0;
}

static void insert_char(term* t, char c)
{
    if (t->in_len >= LINE_MAX - 1) {
        return;
    }
    for (int i = t->in_len; i > t->in_cur; i--) {
        t->in[i] = t->in[i - 1];
    }
    t->in[t->in_cur] = c;
    t->in_len++;
    t->in_cur++;
    t->hist_browse = -1;
}

static void delete_before(term* t)
{
    if (t->in_cur == 0) {
        return;
    }
    for (int i = t->in_cur - 1; i < t->in_len - 1; i++) {
        t->in[i] = t->in[i + 1];
    }
    t->in_len--;
    t->in_cur--;
    t->hist_browse = -1;
}

static void delete_range(term* t, int from, int to) // remove [from, to)
{
    if (from < 0 || to > t->in_len || from >= to) {
        return;
    }
    int n = to - from;
    for (int i = from; i < t->in_len - n; i++) {
        t->in[i] = t->in[i + n];
    }
    t->in_len -= n;
    if (t->in_cur > from) {
        t->in_cur = (t->in_cur > to) ? t->in_cur - n : from;
    }
    t->hist_browse = -1;
}

// Dispatch a completed CSI/escape sequence from the input stream.
static void key_seq(term* t, char final)
{
    t->kcsi[t->kcsi_len] = '\0';
    bool word = false; // Ctrl-arrow arrives as e.g. ESC[1;5C
    for (int i = 0; i < t->kcsi_len; i++) {
        if (t->kcsi[i] == '5') {
            word = true;
        }
    }
    switch (final) {
    case 'A':
        hist_prev(t);
        break;
    case 'B':
        hist_next_(t);
        break;
    case 'C':
        t->in_cur = word ? word_right(t, t->in_cur)
                         : (t->in_cur < t->in_len ? t->in_cur + 1 : t->in_cur);
        break;
    case 'D':
        t->in_cur = word ? word_left(t, t->in_cur)
                         : (t->in_cur > 0 ? t->in_cur - 1 : 0);
        break;
    case 'H':
        t->in_cur = 0;
        break;
    case 'F':
        t->in_cur = t->in_len;
        break;
    case '~': // PgUp = ESC[5~, PgDn = ESC[6~ -> scroll the scrollback
        if (t->kcsi[0] == '5') {
            t->scroll_off += 10;
        } else if (t->kcsi[0] == '6') {
            t->scroll_off -= 10;
            if (t->scroll_off < 0) {
                t->scroll_off = 0;
            }
        }
        break;
    default:
        break;
    }
}

void term_key(term* t, int c)
{
    if (t->kesc == 1) {
        if (c == '[' || c == 'O') {
            t->kesc = 2;
            t->kcsi_len = 0;
        } else if (c == 'b') {
            t->in_cur = word_left(t, t->in_cur);
            t->kesc = 0;
        } else if (c == 'f') {
            t->in_cur = word_right(t, t->in_cur);
            t->kesc = 0;
        } else {
            t->kesc = 0;
        }
        return;
    }
    if (t->kesc == 2) {
        if (c >= 0x40 && c <= 0x7e) {
            key_seq(t, (char)c);
            t->kesc = 0;
        } else if (t->kcsi_len < (int)sizeof(t->kcsi) - 1) {
            t->kcsi[t->kcsi_len++] = (char)c;
        }
        return;
    }
    switch (c) {
    case 27:
        t->kesc = 1;
        break;
    case '\r':
    case '\n':
        submit(t);
        break;
    case 0x7f:
    case 0x08:
        delete_before(t);
        break;
    case 0x01: // Ctrl-A
        t->in_cur = 0;
        break;
    case 0x05: // Ctrl-E
        t->in_cur = t->in_len;
        break;
    case 0x15: // Ctrl-U: kill to start of line
        delete_range(t, 0, t->in_cur);
        t->in_cur = 0;
        break;
    case 0x0b: // Ctrl-K: kill to end of line
        t->in_len = t->in_cur;
        t->in[t->in_len] = '\0';
        break;
    case 0x17: // Ctrl-W: kill the word before the cursor
        delete_range(t, word_left(t, t->in_cur), t->in_cur);
        break;
    default:
        if (c >= 32 && c < 127) {
            insert_char(t, (char)c);
        }
        break;
    }
}

// --- rendering --------------------------------------------------------------

static mu_Color rgb(uint32_t c)
{
    return mu_color((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, 255);
}

// Draw one scrollback line as microui text commands, grouped into runs of the
// same colour (blank cells still advance the column, preserving alignment).
static void draw_grid_line(term* t, mu_Context* ctx, int abs_row, int x, int y,
                           int cols)
{
    Cell* l = line_at(t, abs_row);
    int last = -1;
    for (int c = 0; c < cols && c < TCOLS; c++) {
        if (l[c].ch != ' ' && l[c].ch != 0) {
            last = c;
        }
    }
    int c = 0;
    char run[TCOLS + 1];
    while (c <= last) {
        uint8_t color = l[c].color;
        int start = c;
        int n = 0;
        while (c <= last && l[c].color == color) {
            run[n++] = l[c].ch ? l[c].ch : ' ';
            c++;
        }
        mu_draw_text(ctx, NULL, run, n, mu_vec2(x + start * GW, y),
                     rgb(palette[color < C_N ? color : 0]));
    }
}

// Draw an ANSI-coloured string (used for the live, highlighted input line).
static void draw_ansi(mu_Context* ctx, const char* s, int x, int y)
{
    uint8_t color = C_DEF;
    char run[LINE_MAX];
    int n = 0;
    int col = 0;
    int esc = 0;
    char csi[16];
    int csi_len = 0;
    for (int i = 0; s[i]; i++) {
        char ch = s[i];
        if (esc == 1) {
            esc = (ch == '[') ? 2 : 0;
            csi_len = 0;
            continue;
        }
        if (esc == 2) {
            if (ch >= 0x40 && ch <= 0x7e) {
                if (ch == 'm') {
                    csi[csi_len] = '\0';
                    if (n > 0) {
                        mu_draw_text(ctx, NULL, run, n,
                                     mu_vec2(x + (col - n) * GW, y),
                                     rgb(palette[color]));
                        n = 0;
                    }
                    color = sgr_to_color(atoi_n(csi, csi_len));
                }
                esc = 0;
            } else if (csi_len < (int)sizeof(csi) - 1) {
                csi[csi_len++] = ch;
            }
            continue;
        }
        if (ch == 27) {
            esc = 1;
            continue;
        }
        if (n < LINE_MAX - 1) {
            run[n++] = ch;
            col++;
        }
    }
    if (n > 0) {
        mu_draw_text(ctx, NULL, run, n, mu_vec2(x + (col - n) * GW, y),
                     rgb(palette[color]));
    }
}

void term_build(term* t, mu_Context* ctx)
{
    int W = (int)gfx_width();
    int H = (int)gfx_height();
    int ww = W * 3 / 5;
    int wh = H * 3 / 4;
    // NOCLOSE: the shell window can't be closed out from under you.
    if (!mu_begin_window_ex(ctx, "Terminal", mu_rect(32, 40, ww, wh),
                            MU_OPT_NOCLOSE)) {
        return;
    }
    mu_Container* cnt = mu_get_current_container(ctx);
    mu_Rect b = cnt->body;
    mu_draw_rect(ctx, b, rgb(TERM_BG));
    mu_push_clip_rect(ctx, b);

    int cols = b.w / GW;
    int rows = b.h / GH;
    if (rows < 2) {
        rows = 2;
    }
    int text_rows = rows - 1; // last visible row is the input line

    int bottom = t->wrow - t->scroll_off;
    if (bottom < 0) {
        bottom = 0;
    }
    int start = bottom - (text_rows - 1);
    if (start < 0) {
        start = 0;
    }
    int y = b.y;
    for (int L = start; L <= bottom; L++, y += GH) {
        draw_grid_line(t, ctx, L, b.x, y, cols);
    }

    // Input line pinned to the bottom row of the window body.
    int iy = b.y + text_rows * GH;
    int plen = slen(t->prompt);
    mu_draw_text(ctx, NULL, t->prompt, plen, mu_vec2(b.x, iy),
                 rgb(palette[C_DEF]));
    char hl[LINE_MAX * 8];
    highlight_lua(t->in, (size_t)t->in_len, hl, sizeof hl);
    draw_ansi(ctx, hl, b.x + plen * GW, iy);
    int cx = b.x + (plen + t->in_cur) * GW;
    mu_draw_rect(ctx, mu_rect(cx, iy, 2, GH), rgb(0x9ecbff));

    mu_pop_clip_rect(ctx);
    mu_end_window(ctx);
}
