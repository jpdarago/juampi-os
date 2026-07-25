// Windowed terminal (see term.h). Two halves:
//
//   * a scrollback grid of coloured cells, appended to by term_write() which is
//     installed as the console sink — so everything the shell prints (results,
//     errors, and the highlighter's SGR escapes) is captured and rendered
//     inside a microui window instead of straight to the framebuffer.
//   * a live input line with the same editing + history as the classic shell
//     (src/shell.c), highlighted as you type via highlight_lua(); Enter echoes
//     the line into the scrollback and feeds it to luashell_eval().
//
// term_build() draws the whole thing into a microui window each frame.

#include <term.h>
#include <console.h>
#include <luashell.h>
#include <fault.h>
#include <highlight.h>
#include <gfx.h>
#include <utils.h> // memset

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

static Cell grid[TROWS][TCOLS];
static int wrow;       // absolute index of the line being written (monotonic)
static int wcol;       // write column on that line
static uint8_t wcolor; // current SGR colour index
static int scroll_off; // lines scrolled up from the bottom (0 = follow output)

// SGR/escape parser state for the output stream.
static int oesc;
static char ocsi[16];
static int ocsi_len;

static Cell* line_at(int abs_row)
{
    return grid[((abs_row % TROWS) + TROWS) % TROWS];
}

static void clear_line(int abs_row)
{
    Cell* l = line_at(abs_row);
    for (int i = 0; i < TCOLS; i++) {
        l[i].ch = ' ';
        l[i].color = C_DEF;
    }
}

void term_init(void)
{
    for (int r = 0; r < TROWS; r++) {
        clear_line(r);
    }
    wrow = 0;
    wcol = 0;
    wcolor = C_DEF;
    scroll_off = 0;
    oesc = 0;
}

static void term_newline(void)
{
    wrow++;
    wcol = 0;
    clear_line(wrow);
}

static void put_cell(char c)
{
    if (wcol >= TCOLS) {
        term_newline();
    }
    Cell* l = line_at(wrow);
    l[wcol].ch = c;
    l[wcol].color = wcolor;
    wcol++;
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

static void handle_csi(char final)
{
    ocsi[ocsi_len] = '\0';
    switch (final) {
    case 'm': // SGR colour (single code is all the shell emits)
        wcolor = sgr_to_color(atoi_n(ocsi, ocsi_len));
        break;
    case 'K': // erase from the cursor to end of line
        for (int c = wcol; c < TCOLS; c++) {
            line_at(wrow)[c].ch = ' ';
            line_at(wrow)[c].color = C_DEF;
        }
        break;
    case 'J': // clear screen (console_clear sends ESC[2J)
        for (int r = 0; r < TROWS; r++) {
            clear_line(r);
        }
        wrow = 0;
        wcol = 0;
        break;
    case 'H': // cursor home
        wcol = 0;
        break;
    default:
        break;
    }
}

void term_write(char c)
{
    if (oesc == 1) {
        oesc = (c == '[') ? 2 : 0;
        ocsi_len = 0;
        return;
    }
    if (oesc == 2) {
        if (c >= 0x40 && c <= 0x7e) {
            handle_csi(c);
            oesc = 0;
        } else if (ocsi_len < (int)sizeof(ocsi) - 1) {
            ocsi[ocsi_len++] = c;
        }
        return;
    }
    switch (c) {
    case 27:
        oesc = 1;
        return;
    case '\n':
        term_newline();
        break;
    case '\r':
        wcol = 0;
        break;
    case '\b':
        if (wcol > 0) {
            wcol--;
        }
        break;
    case '\t': {
        int stop = (wcol + 8) & ~7;
        while (wcol < stop && wcol < TCOLS) {
            put_cell(' ');
        }
        break;
    }
    default:
        if ((unsigned char)c >= 32) {
            put_cell(c);
        }
        break;
    }
    scroll_off = 0; // any output snaps the view back to the bottom
}

// --- input line + history ---------------------------------------------------

static char in[LINE_MAX];
static int in_len, in_cur;
static const char* prompt = "lua> ";
static int cont; // continuation (multi-line statement in progress)

static char history[HIST_MAX][LINE_MAX];
static int hist_count, hist_next;
static int hist_browse = -1; // -1 = editing; else steps back from newest
static char hist_saved[LINE_MAX];

// Input-stream escape parser state (separate from the output one).
static int kesc;
static char kcsi[16];
static int kcsi_len;

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
static int word_left(int cur)
{
    while (cur > 0 && !is_word(in[cur - 1])) {
        cur--;
    }
    while (cur > 0 && is_word(in[cur - 1])) {
        cur--;
    }
    return cur;
}
static int word_right(int cur)
{
    while (cur < in_len && !is_word(in[cur])) {
        cur++;
    }
    while (cur < in_len && is_word(in[cur])) {
        cur++;
    }
    return cur;
}

static void hist_add(const char* line)
{
    if (line[0] == '\0') {
        return;
    }
    if (hist_count > 0) {
        int last = (hist_next - 1 + HIST_MAX) % HIST_MAX;
        int i = 0;
        while (history[last][i] && history[last][i] == line[i]) {
            i++;
        }
        if (history[last][i] == line[i]) {
            return; // identical to the previous entry
        }
    }
    int i = 0;
    for (; line[i] && i < LINE_MAX - 1; i++) {
        history[hist_next][i] = line[i];
    }
    history[hist_next][i] = '\0';
    hist_next = (hist_next + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) {
        hist_count++;
    }
}

static void load_line(const char* s)
{
    int i = 0;
    for (; s[i] && i < LINE_MAX - 1; i++) {
        in[i] = s[i];
    }
    in_len = i;
    in_cur = i;
    in[in_len] = '\0';
}

static void hist_prev(void)
{
    if (hist_count == 0) {
        return;
    }
    if (hist_browse == -1) {
        in[in_len] = '\0';
        int j = 0;
        for (; in[j]; j++) {
            hist_saved[j] = in[j];
        }
        hist_saved[j] = '\0';
        hist_browse = 0;
    } else if (hist_browse < hist_count - 1) {
        hist_browse++;
    }
    int idx = (hist_next - 1 - hist_browse + 2 * HIST_MAX) % HIST_MAX;
    load_line(history[idx]);
}

static void hist_next_(void)
{
    if (hist_browse == -1) {
        return;
    }
    if (hist_browse > 0) {
        hist_browse--;
        int idx = (hist_next - 1 - hist_browse + 2 * HIST_MAX) % HIST_MAX;
        load_line(history[idx]);
    } else {
        hist_browse = -1;
        load_line(hist_saved);
    }
}

static void submit(void)
{
    in[in_len] = '\0';
    // Echo the prompt + highlighted line into the scrollback (through the
    // console, so it is also mirrored to serial), then evaluate.
    static char hl[LINE_MAX * 8];
    console_print(prompt);
    highlight_lua(in, (size_t)in_len, hl, sizeof hl);
    console_print(hl);
    console_print("\033[0m\n");

    hist_add(in);
    hist_browse = -1;

    fault_armed = true;
    cont = luashell_eval(in);
    fault_armed = false;

    prompt = cont ? "  >> " : "lua> ";
    in_len = 0;
    in_cur = 0;
    in[0] = '\0';
    scroll_off = 0;
}

static void insert_char(char c)
{
    if (in_len >= LINE_MAX - 1) {
        return;
    }
    for (int i = in_len; i > in_cur; i--) {
        in[i] = in[i - 1];
    }
    in[in_cur] = c;
    in_len++;
    in_cur++;
    hist_browse = -1;
}

static void delete_before(void)
{
    if (in_cur == 0) {
        return;
    }
    for (int i = in_cur - 1; i < in_len - 1; i++) {
        in[i] = in[i + 1];
    }
    in_len--;
    in_cur--;
    hist_browse = -1;
}

static void delete_range(int from, int to) // remove [from, to)
{
    if (from < 0 || to > in_len || from >= to) {
        return;
    }
    int n = to - from;
    for (int i = from; i < in_len - n; i++) {
        in[i] = in[i + n];
    }
    in_len -= n;
    if (in_cur > from) {
        in_cur = (in_cur > to) ? in_cur - n : from;
    }
    hist_browse = -1;
}

// Dispatch a completed CSI/escape sequence from the input stream.
static void key_seq(char final)
{
    kcsi[kcsi_len] = '\0';
    bool word = false; // Ctrl-arrow arrives as e.g. ESC[1;5C
    for (int i = 0; i < kcsi_len; i++) {
        if (kcsi[i] == '5') {
            word = true;
        }
    }
    switch (final) {
    case 'A':
        hist_prev();
        break;
    case 'B':
        hist_next_();
        break;
    case 'C':
        in_cur = word ? word_right(in_cur)
                      : (in_cur < in_len ? in_cur + 1 : in_cur);
        break;
    case 'D':
        in_cur = word ? word_left(in_cur) : (in_cur > 0 ? in_cur - 1 : 0);
        break;
    case 'H':
        in_cur = 0;
        break;
    case 'F':
        in_cur = in_len;
        break;
    case '~': // PgUp = ESC[5~, PgDn = ESC[6~ -> scroll the scrollback
        if (kcsi[0] == '5') {
            scroll_off += 10;
        } else if (kcsi[0] == '6') {
            scroll_off -= 10;
            if (scroll_off < 0) {
                scroll_off = 0;
            }
        }
        break;
    default:
        break;
    }
}

void term_key(int c)
{
    if (kesc == 1) {
        if (c == '[' || c == 'O') {
            kesc = 2;
            kcsi_len = 0;
        } else if (c == 'b') {
            in_cur = word_left(in_cur);
            kesc = 0;
        } else if (c == 'f') {
            in_cur = word_right(in_cur);
            kesc = 0;
        } else {
            kesc = 0;
        }
        return;
    }
    if (kesc == 2) {
        if (c >= 0x40 && c <= 0x7e) {
            key_seq((char)c);
            kesc = 0;
        } else if (kcsi_len < (int)sizeof(kcsi) - 1) {
            kcsi[kcsi_len++] = (char)c;
        }
        return;
    }
    switch (c) {
    case 27:
        kesc = 1;
        break;
    case '\r':
    case '\n':
        submit();
        break;
    case 0x7f:
    case 0x08:
        delete_before();
        break;
    case 0x01: // Ctrl-A
        in_cur = 0;
        break;
    case 0x05: // Ctrl-E
        in_cur = in_len;
        break;
    case 0x15: // Ctrl-U: kill to start of line
        delete_range(0, in_cur);
        in_cur = 0;
        break;
    case 0x0b: // Ctrl-K: kill to end of line
        in_len = in_cur;
        in[in_len] = '\0';
        break;
    case 0x17: // Ctrl-W: kill the word before the cursor
        delete_range(word_left(in_cur), in_cur);
        break;
    default:
        if (c >= 32 && c < 127) {
            insert_char((char)c);
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
static void draw_grid_line(mu_Context* ctx, int abs_row, int x, int y, int cols)
{
    Cell* l = line_at(abs_row);
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

void term_build(mu_Context* ctx)
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

    int bottom = wrow - scroll_off;
    if (bottom < 0) {
        bottom = 0;
    }
    int start = bottom - (text_rows - 1);
    if (start < 0) {
        start = 0;
    }
    int y = b.y;
    for (int L = start; L <= bottom; L++, y += GH) {
        draw_grid_line(ctx, L, b.x, y, cols);
    }

    // Input line pinned to the bottom row of the window body.
    int iy = b.y + text_rows * GH;
    int plen = slen(prompt);
    mu_draw_text(ctx, NULL, prompt, plen, mu_vec2(b.x, iy),
                 rgb(palette[C_DEF]));
    static char hl[LINE_MAX * 8];
    highlight_lua(in, (size_t)in_len, hl, sizeof hl);
    draw_ansi(ctx, hl, b.x + plen * GW, iy);
    int cx = b.x + (plen + in_cur) * GW;
    mu_draw_rect(ctx, mu_rect(cx, iy, 2, GH), rgb(0x9ecbff));

    mu_pop_clip_rect(ctx);
    mu_end_window(ctx);
}
