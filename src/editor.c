// Full-screen Lua editor (see editor.h). A line-array buffer, redrawn each
// keystroke into one ANSI frame so the framebuffer terminal and a serial
// terminal render identically. Each visible line is colorized with the same
// highlight_lua() the REPL line editor uses.

#include <editor.h>
#include <console.h>
#include <highlight.h>
#include <ext2.h>
#include <memory.h>
#include <ui.h> // windowed vim editor: ui_current/ui_text_ansi

#include <printf/printf.h> // snprintf for the status bar
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "microui/microui.h"

#define ED_MAX_LINES 1024
#define ED_MAX_COLS 256 // characters per line (long lines are clipped on load)

// The document: nlines lines, each a NUL-terminated string of line_len[i]
// chars.
static char line_buf[ED_MAX_LINES][ED_MAX_COLS + 1];
static size_t line_len[ED_MAX_LINES];
static size_t nlines;

static size_t cx, cy; // cursor position within the buffer (col, row)
static size_t rowoff,
        coloff;    // top-left visible cell (vertical/horizontal scroll)
static bool dirty; // unsaved changes
static char filepath[128];
static const char* status_msg; // transient message shown in the status bar

static size_t term_cols, term_rows; // refreshed from the console each frame

// --- One-frame output buffer ------------------------------------------------
// The whole screen is composed here and written to the console in a single call
// per frame, which keeps redraws flicker-free.
#define SCR_MAX (512 * 1024)
static char scr[SCR_MAX];
static size_t scr_len;

static void s_raw(const char* p, size_t n)
{
    if (scr_len + n > SCR_MAX) {
        return; // frame buffer full; drop the tail (never happens in practice)
    }
    for (size_t i = 0; i < n; i++) {
        scr[scr_len++] = p[i];
    }
}

static void s_puts(const char* z)
{
    size_t n = 0;
    while (z[n]) {
        n++;
    }
    s_raw(z, n);
}

// Append an unsigned decimal (used for the CSI coordinates).
static void s_putu(size_t v)
{
    char tmp[20];
    size_t i = 0;
    if (v == 0) {
        s_raw("0", 1);
        return;
    }
    while (v > 0 && i < sizeof(tmp)) {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    char out[20];
    for (size_t j = 0; j < i; j++) {
        out[j] = tmp[i - 1 - j];
    }
    s_raw(out, i);
}

// Move the cursor to (row, col), 1-based (ANSI CUP).
static void s_moveto(size_t row, size_t col)
{
    s_puts("\033[");
    s_putu(row);
    s_puts(";");
    s_putu(col);
    s_puts("H");
}

// --- Buffer helpers ---------------------------------------------------------

static void set_status(const char* msg)
{
    status_msg = msg;
}

// Parse `size` bytes of text `p` into the line array (splitting on '\n',
// dropping '\r', clipping long lines). Shared by load_file and undo/paste.
static void buffer_from_text(const char* p, size_t size)
{
    nlines = 0;
    size_t col = 0;
    line_len[0] = 0;
    for (size_t j = 0; j < size && nlines < ED_MAX_LINES; j++) {
        char ch = p[j];
        if (ch == '\n') {
            line_buf[nlines][col] = '\0';
            line_len[nlines] = col;
            nlines++;
            col = 0;
            if (nlines < ED_MAX_LINES) {
                line_buf[nlines][0] = '\0';
            }
        } else if (ch == '\r') {
            continue; // drop CR from CRLF files
        } else if (col < ED_MAX_COLS) {
            line_buf[nlines][col++] = ch;
        }
        // Characters past ED_MAX_COLS are clipped.
    }
    // Close the final line unless the file ended exactly on a newline.
    if ((col > 0 || nlines == 0) && nlines < ED_MAX_LINES) {
        line_buf[nlines][col] = '\0';
        line_len[nlines] = col;
        nlines++;
    }
    if (nlines == 0) {
        nlines = 1;
        line_len[0] = 0;
        line_buf[0][0] = '\0';
    }
}

// Load `path` into the line array; a missing file yields a single empty line.
static void load_file(const char* path)
{
    // Copy the path (bounded) for the status bar and for saving.
    size_t i = 0;
    for (; path[i] != '\0' && i < sizeof(filepath) - 1; i++) {
        filepath[i] = path[i];
    }
    filepath[i] = '\0';

    size_t size = 0;
    void* data = ext2_read_path(path, &size);
    if (data == NULL) {
        nlines = 1;
        line_len[0] = 0;
        line_buf[0][0] = '\0';
        return;
    }
    buffer_from_text((const char*)data, size);
    heap_free(heap_default(), data);
}

// Serialize the buffer (lines joined with '\n', trailing newline) and write it
// back to ext2. Returns false on allocation or write failure.
static bool save_file(void)
{
    size_t total = 0;
    for (size_t i = 0; i < nlines; i++) {
        total += line_len[i] + 1; // + '\n'
    }
    char* out = new (&heap_default()->base, char, (ptrdiff_t)total);
    if (out == NULL) {
        return false;
    }
    size_t o = 0;
    for (size_t i = 0; i < nlines; i++) {
        for (size_t j = 0; j < line_len[i]; j++) {
            out[o++] = line_buf[i][j];
        }
        out[o++] = '\n';
    }
    bool ok = ext2_write_file(filepath, out, total);
    heap_free(heap_default(), out);
    if (ok) {
        dirty = false;
    }
    return ok;
}

// Insert a printable character at the cursor.
static void insert_char(char ch)
{
    if (line_len[cy] >= ED_MAX_COLS) {
        return; // line full
    }
    char* line = line_buf[cy];
    for (size_t i = line_len[cy]; i > cx; i--) {
        line[i] = line[i - 1];
    }
    line[cx] = ch;
    line_len[cy]++;
    line[line_len[cy]] = '\0';
    cx++;
    dirty = true;
}

// Split the current line at the cursor, pushing the tail down into a new line.
static void insert_newline(void)
{
    if (nlines >= ED_MAX_LINES) {
        return;
    }
    // Make room for one more line after cy.
    for (size_t i = nlines; i > cy + 1; i--) {
        for (size_t j = 0; j <= line_len[i - 1]; j++) {
            line_buf[i][j] = line_buf[i - 1][j];
        }
        line_len[i] = line_len[i - 1];
    }
    nlines++;

    // Move the tail [cx, len) of the current line into the new line cy+1.
    size_t tail = line_len[cy] - cx;
    for (size_t j = 0; j < tail; j++) {
        line_buf[cy + 1][j] = line_buf[cy][cx + j];
    }
    line_buf[cy + 1][tail] = '\0';
    line_len[cy + 1] = tail;

    line_buf[cy][cx] = '\0';
    line_len[cy] = cx;

    cy++;
    cx = 0;
    dirty = true;
}

// Append line `src` onto the end of line `dst` (bounded), used when joining.
static void join_into(size_t dst, size_t src)
{
    size_t room = ED_MAX_COLS - line_len[dst];
    size_t n = line_len[src];
    if (n > room) {
        n = room;
    }
    for (size_t j = 0; j < n; j++) {
        line_buf[dst][line_len[dst] + j] = line_buf[src][j];
    }
    line_len[dst] += n;
    line_buf[dst][line_len[dst]] = '\0';
}

// Remove line `row` by shifting the lines below it up.
static void delete_line(size_t row)
{
    for (size_t i = row; i + 1 < nlines; i++) {
        for (size_t j = 0; j <= line_len[i + 1]; j++) {
            line_buf[i][j] = line_buf[i + 1][j];
        }
        line_len[i] = line_len[i + 1];
    }
    nlines--;
}

// Delete the character before the cursor, joining with the previous line at
// BOL.
static void do_backspace(void)
{
    if (cx > 0) {
        char* line = line_buf[cy];
        for (size_t i = cx - 1; i + 1 < line_len[cy]; i++) {
            line[i] = line[i + 1];
        }
        line_len[cy]--;
        line[line_len[cy]] = '\0';
        cx--;
        dirty = true;
    } else if (cy > 0) {
        size_t prev_len = line_len[cy - 1];
        join_into(cy - 1, cy);
        delete_line(cy);
        cy--;
        cx = prev_len;
        dirty = true;
    }
}

// Delete the character at the cursor, joining the next line at EOL.
static void do_delete(void)
{
    if (cx < line_len[cy]) {
        char* line = line_buf[cy];
        for (size_t i = cx; i + 1 < line_len[cy]; i++) {
            line[i] = line[i + 1];
        }
        line_len[cy]--;
        line[line_len[cy]] = '\0';
        dirty = true;
    } else if (cy + 1 < nlines) {
        join_into(cy, cy + 1);
        delete_line(cy + 1);
        dirty = true;
    }
}

// Clamp the cursor column to the current line's length (after vertical motion).
static void clamp_cx(void)
{
    if (cx > line_len[cy]) {
        cx = line_len[cy];
    }
}

// --- Rendering --------------------------------------------------------------

// Effective editable width, capped so a very wide terminal can't overrun the
// per-line color scratch buffer below.
static size_t view_width(void)
{
    return term_cols > ED_MAX_COLS ? ED_MAX_COLS : term_cols;
}

// Adjust the scroll offsets so the cursor is on screen.
static void scroll_to_cursor(void)
{
    size_t textrows = term_rows - 1;
    size_t width = view_width();
    if (cy < rowoff) {
        rowoff = cy;
    }
    if (cy >= rowoff + textrows) {
        rowoff = cy - textrows + 1;
    }
    if (cx < coloff) {
        coloff = cx;
    }
    if (cx >= coloff + width) {
        coloff = cx - width + 1;
    }
}

static void render_line(size_t row)
{
    static char colored[ED_MAX_COLS * 12];
    size_t len = line_len[row];
    size_t width = view_width();
    if (coloff >= len) {
        return; // scrolled entirely past this (short) line
    }
    size_t vis = len - coloff;
    if (vis > width) {
        vis = width;
    }
    highlight_lua(line_buf[row] + coloff, vis, colored, sizeof colored);
    s_puts(colored);
}

static void draw_status(void)
{
    s_moveto(term_rows, 1);
    s_puts("\033[7m"); // reverse video
    size_t used = 0;

    s_raw(" ", 1);
    used++;
    for (size_t i = 0; filepath[i] && used < term_cols; i++, used++) {
        s_raw(&filepath[i], 1);
    }
    if (dirty && used + 2 < term_cols) {
        s_puts(" *");
        used += 2;
    }

    // Right-aligned "Ln x, Col y" plus a transient message / key hints.
    const char* right = status_msg ? status_msg : "^S save  ^X run  ^Q quit";
    // Compose "  Ln <cy+1>, Col <cx+1>  <right> "
    char pos[64];
    size_t k = 0;
    const char* lnp = "  Ln ";
    for (const char* q = lnp; *q && k < sizeof(pos) - 1; q++) {
        pos[k++] = *q;
    }
    // cy+1
    {
        size_t v = cy + 1;
        char t[20];
        size_t n = 0;
        if (v == 0) {
            t[n++] = '0';
        }
        while (v > 0 && n < sizeof(t)) {
            t[n++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (n > 0 && k < sizeof(pos) - 1) {
            pos[k++] = t[--n];
        }
    }
    const char* colp = ", Col ";
    for (const char* q = colp; *q && k < sizeof(pos) - 1; q++) {
        pos[k++] = *q;
    }
    {
        size_t v = cx + 1;
        char t[20];
        size_t n = 0;
        if (v == 0) {
            t[n++] = '0';
        }
        while (v > 0 && n < sizeof(t)) {
            t[n++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (n > 0 && k < sizeof(pos) - 1) {
            pos[k++] = t[--n];
        }
    }
    pos[k] = '\0';

    // Right segment = pos + "  " + right; pad the middle with spaces so it sits
    // flush against the right edge (truncated if the terminal is narrow).
    size_t rlen = k;
    for (const char* q = right; *q; q++) {
        rlen++;
    }
    rlen += 2; // the "  " between pos and right
    while (used + rlen < term_cols) {
        s_raw(" ", 1);
        used++;
    }
    if (used < term_cols) {
        s_puts(pos);
        s_puts("  ");
        s_puts(right);
        used += rlen;
    }
    while (used < term_cols) {
        s_raw(" ", 1);
        used++;
    }
    s_puts("\033[0m");
}

static void render(void)
{
    console_dimensions(&term_cols, &term_rows);
    if (term_rows < 2) {
        term_rows = 2;
    }
    scroll_to_cursor();

    scr_len = 0;
    s_puts("\033[?25l"); // hide cursor while composing
    s_puts("\033[H");

    size_t textrows = term_rows - 1;
    for (size_t y = 0; y < textrows; y++) {
        size_t row = rowoff + y;
        s_moveto(y + 1, 1);
        if (row < nlines) {
            render_line(row);
        }
        s_puts("\033[K"); // clear the rest of the row
    }

    draw_status();

    // Place the hardware cursor at the edit position and reveal it.
    s_moveto((cy - rowoff) + 1, (cx - coloff) + 1);
    s_puts("\033[?25h");

    console_write(scr, scr_len);
}

// --- Input ------------------------------------------------------------------

enum {
    KEY_UP = 0x100,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_DEL,
    KEY_ENTER,
    KEY_BACKSPACE,
};

// Tiny string compare for the CSI parameter dispatch.
static bool seq_eq(const char* a, const char* b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

// Read the tail of an ESC '[' / ESC 'O' sequence: parameter/intermediate bytes
// into `params`, final byte into *final_out.
static bool read_seq(char* params, size_t cap, char* final_out)
{
    size_t k = 0;
    for (;;) {
        int ch = console_getch();
        if (ch < 0) {
            return false;
        }
        if (ch >= 0x40 && ch <= 0x7E) {
            params[k] = '\0';
            *final_out = (char)ch;
            return true;
        }
        if (k + 1 < cap) {
            params[k++] = (char)ch;
        }
    }
}

// Read one logical key: a byte for printable/control keys, or a KEY_* code.
static int read_key(void)
{
    int c = console_getch();
    if (c == '\r' || c == '\n') {
        return KEY_ENTER;
    }
    if (c == 0x7F || c == 0x08) {
        return KEY_BACKSPACE;
    }
    if (c != 27) {
        return c;
    }
    int c2 = console_getch();
    if (c2 != '[' && c2 != 'O') {
        return -1; // lone ESC or unhandled Alt-combo
    }
    char params[8];
    char fin;
    if (!read_seq(params, sizeof params, &fin)) {
        return -1;
    }
    switch (fin) {
    case 'A':
        return KEY_UP;
    case 'B':
        return KEY_DOWN;
    case 'C':
        return KEY_RIGHT;
    case 'D':
        return KEY_LEFT;
    case 'H':
        return KEY_HOME;
    case 'F':
        return KEY_END;
    case '~':
        if (seq_eq(params, "3")) {
            return KEY_DEL;
        }
        if (seq_eq(params, "1") || seq_eq(params, "7")) {
            return KEY_HOME;
        }
        if (seq_eq(params, "4") || seq_eq(params, "8")) {
            return KEY_END;
        }
        if (seq_eq(params, "5")) {
            return KEY_PGUP;
        }
        if (seq_eq(params, "6")) {
            return KEY_PGDN;
        }
        return -1;
    default:
        return -1;
    }
}

#define CTRL(c) ((c) & 0x1f)

int editor_run(const char* path)
{
    load_file(path);
    cx = cy = rowoff = coloff = 0;
    dirty = false;
    status_msg = NULL;
    bool quit_armed = false; // set after a Ctrl-Q on a dirty buffer

    for (;;) {
        render();
        int k = read_key();

        // Any key other than a repeated Ctrl-Q cancels the pending-quit
        // warning.
        if (k != CTRL('q')) {
            quit_armed = false;
            if (status_msg && k != -1) {
                status_msg = NULL;
            }
        }

        switch (k) {
        case CTRL('q'):
            if (dirty && !quit_armed) {
                set_status("Unsaved changes — Ctrl-Q again to quit");
                quit_armed = true;
                break;
            }
            console_clear();
            return EDITOR_QUIT;
        case CTRL('s'):
            set_status(save_file() ? "Saved" : "Save failed");
            break;
        case CTRL('x'):
            if (save_file()) {
                console_clear();
                return EDITOR_RUN;
            }
            set_status("Save failed");
            break;
        case KEY_UP:
            if (cy > 0) {
                cy--;
                clamp_cx();
            }
            break;
        case KEY_DOWN:
            if (cy + 1 < nlines) {
                cy++;
                clamp_cx();
            }
            break;
        case KEY_LEFT:
            if (cx > 0) {
                cx--;
            } else if (cy > 0) {
                cy--;
                cx = line_len[cy];
            }
            break;
        case KEY_RIGHT:
            if (cx < line_len[cy]) {
                cx++;
            } else if (cy + 1 < nlines) {
                cy++;
                cx = 0;
            }
            break;
        case KEY_HOME:
        case CTRL('a'):
            cx = 0;
            break;
        case KEY_END:
        case CTRL('e'):
            cx = line_len[cy];
            break;
        case KEY_PGUP: {
            size_t page = term_rows - 1;
            cy = cy > page ? cy - page : 0;
            clamp_cx();
            break;
        }
        case KEY_PGDN: {
            size_t page = term_rows - 1;
            cy = cy + page < nlines ? cy + page : nlines - 1;
            clamp_cx();
            break;
        }
        case KEY_ENTER:
            insert_newline();
            break;
        case KEY_BACKSPACE:
            do_backspace();
            break;
        case KEY_DEL:
            do_delete();
            break;
        default:
            if (k >= 0x20 && k < 0x7F) {
                insert_char((char)k);
            }
            break;
        }
    }
}

// ============================================================================
// Vim-style modal layer for the windowed editor (ui_edit in ui.c). Shares the
// buffer + ops above; the classic editor_run() stays as the headless fallback.
// ============================================================================

#define VGW 8
#define VGH 16

enum { M_NORMAL, M_INSERT, M_COMMAND, M_SEARCH };
static int vmode;
static int vcount;     // numeric count prefix being typed (0 = none)
static int vpending;   // pending operator: 'd','c','g','r','y' (0 = none)
static char vcmd[128]; // the ':' command / '/' search being typed
static size_t vcmd_len;
static char vsearch[128]; // last committed search pattern
static size_t vsearch_len;

// Linewise yank register + undo ring, both as serialized text blobs.
static char* vyank;
static size_t vyank_len;
#define VUNDO_MAX 32
static char* vundo[VUNDO_MAX];
static size_t vundo_sz[VUNDO_MAX];
static size_t vundo_cx[VUNDO_MAX], vundo_cy[VUNDO_MAX];
static int vundo_head, vundo_count;

static bool ed_isword(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}
static bool ed_isspace(char c)
{
    return c == ' ' || c == '\t';
}

// Serialize the buffer to a heap blob (lines joined with '\n'); NULL on OOM.
static char* serialize_buf(size_t* out_len)
{
    size_t total = 0;
    for (size_t i = 0; i < nlines; i++) {
        total += line_len[i] + 1;
    }
    char* b = new (&heap_default()->base, char, (ptrdiff_t)(total ? total : 1));
    if (b == NULL) {
        *out_len = 0;
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < nlines; i++) {
        for (size_t j = 0; j < line_len[i]; j++) {
            b[o++] = line_buf[i][j];
        }
        b[o++] = '\n';
    }
    *out_len = o;
    return b;
}

// Snapshot the buffer for undo before a modifying command group.
static void push_undo(void)
{
    size_t len;
    char* blob = serialize_buf(&len);
    if (blob == NULL) {
        return;
    }
    if (vundo_count == VUNDO_MAX) {
        int oldest = (vundo_head - vundo_count + 2 * VUNDO_MAX) % VUNDO_MAX;
        heap_free(heap_default(), vundo[oldest]);
        vundo_count--;
    }
    vundo[vundo_head] = blob;
    vundo_sz[vundo_head] = len;
    vundo_cx[vundo_head] = cx;
    vundo_cy[vundo_head] = cy;
    vundo_head = (vundo_head + 1) % VUNDO_MAX;
    vundo_count++;
}

static void do_undo(void)
{
    if (vundo_count == 0) {
        set_status("Already at oldest change");
        return;
    }
    vundo_head = (vundo_head - 1 + VUNDO_MAX) % VUNDO_MAX;
    vundo_count--;
    char* blob = vundo[vundo_head];
    buffer_from_text(blob, vundo_sz[vundo_head]);
    cx = vundo_cx[vundo_head];
    cy = vundo_cy[vundo_head];
    heap_free(heap_default(), blob);
    vundo[vundo_head] = NULL;
    if (cy >= nlines) {
        cy = nlines ? nlines - 1 : 0;
    }
    clamp_cx();
    dirty = true;
}

// --- motions ----------------------------------------------------------------

static void vim_w(void)
{
    size_t len = line_len[cy];
    if (cx < len && !ed_isspace(line_buf[cy][cx])) {
        bool word = ed_isword(line_buf[cy][cx]);
        while (cx < len && !ed_isspace(line_buf[cy][cx]) &&
               ed_isword(line_buf[cy][cx]) == word) {
            cx++;
        }
    }
    for (;;) {
        len = line_len[cy];
        if (cx >= len) {
            if (cy + 1 < nlines) {
                cy++;
                cx = 0;
            } else {
                cx = len;
                return;
            }
        } else if (ed_isspace(line_buf[cy][cx])) {
            cx++;
        } else {
            return;
        }
    }
}

static void vim_b(void)
{
    if (cx == 0) {
        if (cy == 0) {
            return;
        }
        cy--;
        cx = line_len[cy];
    } else {
        cx--;
    }
    while (cx > 0 && (cx >= line_len[cy] || ed_isspace(line_buf[cy][cx]))) {
        cx--;
    }
    if (cx < line_len[cy] && !ed_isspace(line_buf[cy][cx])) {
        bool word = ed_isword(line_buf[cy][cx]);
        while (cx > 0 && !ed_isspace(line_buf[cy][cx - 1]) &&
               ed_isword(line_buf[cy][cx - 1]) == word) {
            cx--;
        }
    }
}

static void vim_e(void)
{
    size_t len = line_len[cy];
    if (cx + 1 >= len) {
        if (cy + 1 < nlines) {
            cy++;
            cx = 0;
        }
    } else {
        cx++;
    }
    for (;;) {
        len = line_len[cy];
        if (cx >= len) {
            if (cy + 1 < nlines) {
                cy++;
                cx = 0;
            } else {
                return;
            }
        } else if (ed_isspace(line_buf[cy][cx])) {
            cx++;
        } else {
            break;
        }
    }
    len = line_len[cy];
    if (cx < len && !ed_isspace(line_buf[cy][cx])) {
        bool word = ed_isword(line_buf[cy][cx]);
        while (cx + 1 < len && !ed_isspace(line_buf[cy][cx + 1]) &&
               ed_isword(line_buf[cy][cx + 1]) == word) {
            cx++;
        }
    }
}

// --- edits ------------------------------------------------------------------

// Delete the char at the cursor without joining lines (vim 'x').
static void del_char(void)
{
    if (cx >= line_len[cy]) {
        return;
    }
    char* line = line_buf[cy];
    for (size_t i = cx; i + 1 < line_len[cy]; i++) {
        line[i] = line[i + 1];
    }
    line_len[cy]--;
    line[line_len[cy]] = '\0';
    dirty = true;
}

// Delete from the cursor to the start of the next word, on the current line
// (vim 'dw').
static void del_word(void)
{
    size_t len = line_len[cy];
    size_t e = cx;
    if (e < len) {
        if (ed_isspace(line_buf[cy][e])) {
            while (e < len && ed_isspace(line_buf[cy][e])) {
                e++;
            }
        } else {
            bool word = ed_isword(line_buf[cy][e]);
            while (e < len && !ed_isspace(line_buf[cy][e]) &&
                   ed_isword(line_buf[cy][e]) == word) {
                e++;
            }
            while (e < len && ed_isspace(line_buf[cy][e])) {
                e++;
            }
        }
    }
    char* line = line_buf[cy];
    for (size_t i = e; i <= line_len[cy]; i++) {
        line[cx + (i - e)] = line[i];
    }
    line_len[cy] -= (e - cx);
    dirty = true;
}

static void vim_join(void)
{
    if (cy + 1 >= nlines) {
        return;
    }
    size_t at = line_len[cy];
    size_t s = 0;
    while (s < line_len[cy + 1] && ed_isspace(line_buf[cy + 1][s])) {
        s++;
    }
    if (line_len[cy] > 0 && s < line_len[cy + 1] && at < ED_MAX_COLS) {
        line_buf[cy][at++] = ' ';
    }
    for (size_t j = s; j < line_len[cy + 1] && at < ED_MAX_COLS; j++) {
        line_buf[cy][at++] = line_buf[cy + 1][j];
    }
    line_buf[cy][at] = '\0';
    line_len[cy] = at;
    delete_line(cy + 1);
    dirty = true;
}

// Copy `count` lines starting at `from` into the yank register (linewise).
static void yank_lines(size_t from, size_t count)
{
    if (vyank != NULL) {
        heap_free(heap_default(), vyank);
        vyank = NULL;
        vyank_len = 0;
    }
    size_t total = 0;
    for (size_t i = 0; i < count && from + i < nlines; i++) {
        total += line_len[from + i] + 1;
    }
    vyank = new (&heap_default()->base, char, (ptrdiff_t)(total ? total : 1));
    if (vyank == NULL) {
        return;
    }
    size_t o = 0;
    for (size_t i = 0; i < count && from + i < nlines; i++) {
        for (size_t j = 0; j < line_len[from + i]; j++) {
            vyank[o++] = line_buf[from + i][j];
        }
        vyank[o++] = '\n';
    }
    vyank_len = o;
}

static void insert_line_at(size_t at, const char* text, size_t len)
{
    if (nlines >= ED_MAX_LINES) {
        return;
    }
    for (size_t i = nlines; i > at; i--) {
        for (size_t j = 0; j <= line_len[i - 1]; j++) {
            line_buf[i][j] = line_buf[i - 1][j];
        }
        line_len[i] = line_len[i - 1];
    }
    size_t n = len > ED_MAX_COLS ? ED_MAX_COLS : len;
    for (size_t j = 0; j < n; j++) {
        line_buf[at][j] = text[j];
    }
    line_buf[at][n] = '\0';
    line_len[at] = n;
    nlines++;
}

// Paste the (linewise) yank register below or above the current line.
static void paste_lines(bool below)
{
    if (vyank == NULL || vyank_len == 0) {
        return;
    }
    size_t at = below ? cy + 1 : cy;
    size_t first = at;
    size_t start = 0;
    for (size_t i = 0; i < vyank_len; i++) {
        if (vyank[i] == '\n') {
            insert_line_at(at, vyank + start, i - start);
            at++;
            start = i + 1;
        }
    }
    cy = first;
    cx = 0;
    dirty = true;
}

// Linear substring search from just after the cursor (dir +1) or before it
// (dir -1), wrapping. Moves the cursor to a match or reports not found.
static void do_search(int dir)
{
    if (vsearch_len == 0) {
        return;
    }
    for (size_t step = 1; step <= nlines; step++) {
        size_t row = (dir > 0) ? (cy + step) % nlines
                               : (cy + nlines - step) % nlines;
        const char* line = line_buf[row];
        size_t len = line_len[row];
        for (size_t c = 0; c + vsearch_len <= len; c++) {
            size_t m = 0;
            while (m < vsearch_len && line[c + m] == vsearch[m]) {
                m++;
            }
            if (m == vsearch_len) {
                cy = row;
                cx = c;
                return;
            }
        }
    }
    set_status("Pattern not found");
}

// --- dispatch ---------------------------------------------------------------

static int run_command(void)
{
    vcmd[vcmd_len] = '\0';
    const char* c = vcmd;
    // A bare number jumps to that line.
    if (c[0] >= '1' && c[0] <= '9') {
        size_t n = 0;
        for (size_t i = 0; c[i] >= '0' && c[i] <= '9'; i++) {
            n = n * 10 + (size_t)(c[i] - '0');
        }
        if (n >= 1 && n <= nlines) {
            cy = n - 1;
            clamp_cx();
        }
        return EDITOR_CONTINUE;
    }
    bool w = false, q = false, force = false, run = false;
    for (size_t i = 0; c[i]; i++) {
        if (c[i] == 'w') {
            w = true;
        } else if (c[i] == 'q') {
            q = true;
        } else if (c[i] == 'x') {
            w = q = true;
        } else if (c[i] == '!') {
            force = true;
        }
    }
    if (vcmd_len >= 3 && c[0] == 'r' && c[1] == 'u' && c[2] == 'n') {
        run = true;
        w = true;
    }
    if (w) {
        set_status(save_file() ? "Saved" : "Save failed");
        if (!dirty && run) {
            return EDITOR_RUN;
        }
    }
    if (q) {
        if (dirty && !force && !w) {
            set_status("No write since last change (add ! to override)");
            return EDITOR_CONTINUE;
        }
        return EDITOR_QUIT;
    }
    if (!w && !run) {
        set_status("Unknown command");
    }
    return EDITOR_CONTINUE;
}

static int handle_normal(int k)
{
    // Numeric count prefix ('0' is a motion unless a count is in progress).
    if ((k >= '1' && k <= '9') || (k == '0' && vcount > 0)) {
        vcount = vcount * 10 + (k - '0');
        return EDITOR_CONTINUE;
    }
    int cnt = vcount > 0 ? vcount : 1;

    // Pending operators.
    if (vpending) {
        int op = vpending;
        vpending = 0;
        vcount = 0;
        if (op == 'r') {
            if (k >= 0x20 && k < 0x7F && cx < line_len[cy]) {
                push_undo();
                line_buf[cy][cx] = (char)k;
                dirty = true;
            }
        } else if (op == 'g') {
            if (k == 'g') {
                cy = 0;
                clamp_cx();
            }
        } else if (op == 'd') {
            if (k == 'd') {
                push_undo();
                yank_lines(cy, (size_t)cnt);
                if (nlines <= 1) {
                    // Deleting the only line leaves one empty line (like vim).
                    line_len[0] = 0;
                    line_buf[0][0] = '\0';
                    cx = 0;
                } else {
                    for (int i = 0; i < cnt && nlines > 1; i++) {
                        delete_line(cy);
                    }
                    if (cy >= nlines) {
                        cy = nlines - 1;
                    }
                }
                clamp_cx();
            } else if (k == 'w') {
                push_undo();
                del_word();
            }
        } else if (op == 'c') {
            if (k == 'c') {
                push_undo();
                line_len[cy] = 0;
                line_buf[cy][0] = '\0';
                cx = 0;
                vmode = M_INSERT;
            } else if (k == 'w') {
                push_undo();
                del_word();
                vmode = M_INSERT;
            }
        } else if (op == 'y') {
            if (k == 'y') {
                yank_lines(cy, (size_t)cnt);
                set_status("Yanked");
            }
        }
        return EDITOR_CONTINUE;
    }

    vcount = 0;
    switch (k) {
    case 'h':
    case KEY_LEFT:
        for (int i = 0; i < cnt && cx > 0; i++) {
            cx--;
        }
        break;
    case 'l':
    case KEY_RIGHT:
        for (int i = 0; i < cnt && cx < line_len[cy]; i++) {
            cx++;
        }
        break;
    case 'j':
    case KEY_DOWN:
        for (int i = 0; i < cnt && cy + 1 < nlines; i++) {
            cy++;
        }
        clamp_cx();
        break;
    case 'k':
    case KEY_UP:
        for (int i = 0; i < cnt && cy > 0; i++) {
            cy--;
        }
        clamp_cx();
        break;
    case '0':
    case KEY_HOME:
        cx = 0;
        break;
    case '^': {
        size_t i = 0;
        while (i < line_len[cy] && ed_isspace(line_buf[cy][i])) {
            i++;
        }
        cx = i;
        break;
    }
    case '$':
    case KEY_END:
        cx = line_len[cy];
        break;
    case 'w':
        for (int i = 0; i < cnt; i++) {
            vim_w();
        }
        break;
    case 'b':
        for (int i = 0; i < cnt; i++) {
            vim_b();
        }
        break;
    case 'e':
        for (int i = 0; i < cnt; i++) {
            vim_e();
        }
        break;
    case 'G':
        cy = (k == 'G' && cnt > 1) ? (size_t)cnt - 1 : nlines - 1;
        if (cy >= nlines) {
            cy = nlines - 1;
        }
        clamp_cx();
        break;
    case KEY_PGUP:
    case CTRL('u'): {
        size_t p = term_rows > 1 ? (term_rows - 1) / 2 : 1;
        cy = cy > p ? cy - p : 0;
        clamp_cx();
        break;
    }
    case KEY_PGDN:
    case CTRL('d'): {
        size_t p = term_rows > 1 ? (term_rows - 1) / 2 : 1;
        cy = cy + p < nlines ? cy + p : nlines - 1;
        clamp_cx();
        break;
    }
    case 'i':
        push_undo();
        vmode = M_INSERT;
        break;
    case 'a':
        push_undo();
        if (cx < line_len[cy]) {
            cx++;
        }
        vmode = M_INSERT;
        break;
    case 'I':
        push_undo();
        cx = 0;
        vmode = M_INSERT;
        break;
    case 'A':
        push_undo();
        cx = line_len[cy];
        vmode = M_INSERT;
        break;
    case 'o':
        push_undo();
        cx = line_len[cy];
        insert_newline();
        vmode = M_INSERT;
        break;
    case 'O':
        push_undo();
        cx = 0;
        insert_newline();
        if (cy > 0) {
            cy--;
        }
        vmode = M_INSERT;
        break;
    case 'x':
        push_undo();
        for (int i = 0; i < cnt; i++) {
            del_char();
        }
        clamp_cx();
        break;
    case 'D':
        push_undo();
        line_len[cy] = cx;
        line_buf[cy][cx] = '\0';
        dirty = true;
        break;
    case 'C':
        push_undo();
        line_len[cy] = cx;
        line_buf[cy][cx] = '\0';
        vmode = M_INSERT;
        dirty = true;
        break;
    case 'J':
        push_undo();
        vim_join();
        break;
    case 'p':
        push_undo();
        paste_lines(true);
        break;
    case 'P':
        push_undo();
        paste_lines(false);
        break;
    case 'u':
        do_undo();
        break;
    case 'd':
    case 'c':
    case 'g':
    case 'r':
    case 'y':
        vpending = k;
        vcount = cnt > 1 ? cnt : 0; // carry the count to the operator
        break;
    case 'n':
        do_search(1);
        break;
    case 'N':
        do_search(-1);
        break;
    case ':':
        vmode = M_COMMAND;
        vcmd_len = 0;
        vcmd[0] = '\0';
        break;
    case '/':
        vmode = M_SEARCH;
        vcmd_len = 0;
        vcmd[0] = '\0';
        break;
    case CTRL('s'):
        set_status(save_file() ? "Saved" : "Save failed");
        break;
    case CTRL('x'):
        if (save_file()) {
            return EDITOR_RUN;
        }
        set_status("Save failed");
        break;
    case CTRL('q'):
        if (dirty) {
            set_status("Unsaved changes (:q! to discard, :wq to save)");
            break;
        }
        return EDITOR_QUIT;
    default:
        break;
    }
    return EDITOR_CONTINUE;
}

static void handle_insert(int k)
{
    switch (k) {
    case KEY_ENTER:
        insert_newline();
        break;
    case KEY_BACKSPACE:
        do_backspace();
        break;
    case KEY_DEL:
        do_delete();
        break;
    case KEY_LEFT:
        if (cx > 0) {
            cx--;
        }
        break;
    case KEY_RIGHT:
        if (cx < line_len[cy]) {
            cx++;
        }
        break;
    case KEY_UP:
        if (cy > 0) {
            cy--;
            clamp_cx();
        }
        break;
    case KEY_DOWN:
        if (cy + 1 < nlines) {
            cy++;
            clamp_cx();
        }
        break;
    case KEY_HOME:
        cx = 0;
        break;
    case KEY_END:
        cx = line_len[cy];
        break;
    default:
        if (k >= 0x20 && k < 0x7F) {
            insert_char((char)k);
        }
        break;
    }
}

// Esc / mode return.
static void vim_escape(void)
{
    if (vmode == M_INSERT && cx > 0) {
        cx--; // vim steps left leaving insert
    }
    vmode = M_NORMAL;
    vpending = 0;
    vcount = 0;
}

static int handle_line_mode(int k)
{
    if (k == 27) { // Esc cancels
        vmode = M_NORMAL;
        return EDITOR_CONTINUE;
    }
    if (k == KEY_ENTER || k == '\r' || k == '\n') {
        int m = vmode;
        vmode = M_NORMAL;
        if (m == M_SEARCH) {
            for (size_t i = 0; i <= vcmd_len; i++) {
                vsearch[i] = vcmd[i];
            }
            vsearch_len = vcmd_len;
            do_search(1);
            return EDITOR_CONTINUE;
        }
        return run_command();
    }
    if (k == KEY_BACKSPACE) {
        if (vcmd_len > 0) {
            vcmd_len--;
            vcmd[vcmd_len] = '\0';
        } else {
            vmode = M_NORMAL;
        }
        return EDITOR_CONTINUE;
    }
    if (k >= 0x20 && k < 0x7F && vcmd_len < sizeof(vcmd) - 1) {
        vcmd[vcmd_len++] = (char)k;
        vcmd[vcmd_len] = '\0';
    }
    return EDITOR_CONTINUE;
}

// Dispatch one decoded key (a byte or a KEY_* code) by mode.
static int editor_vim_key_logical(int k)
{
    if (k == 27) {
        if (vmode == M_COMMAND || vmode == M_SEARCH) {
            vmode = M_NORMAL;
        } else {
            vim_escape();
        }
        return EDITOR_CONTINUE;
    }
    if (k != -1 && status_msg && vmode == M_NORMAL) {
        status_msg = NULL; // clear a transient message on the next action
    }
    switch (vmode) {
    case M_INSERT:
        handle_insert(k);
        return EDITOR_CONTINUE;
    case M_COMMAND:
    case M_SEARCH:
        return handle_line_mode(k);
    default:
        return handle_normal(k);
    }
}

// Byte-level VT100 decoder feeding editor_vim_key_logical. Handles a lone Esc
// vs an ESC [ ... arrow sequence (resolved by a flush from the loop, byte < 0).
int editor_vim_key(int byte)
{
    static int es;
    static char csi[8];
    static int csi_len;

    if (byte < 0) { // frame flush: resolve a dangling Esc as a lone Esc
        if (es == 1) {
            es = 0;
            return editor_vim_key_logical(27);
        }
        return EDITOR_CONTINUE;
    }
    if (es == 1) {
        if (byte == '[' || byte == 'O') {
            es = 2;
            csi_len = 0;
            return EDITOR_CONTINUE;
        }
        es = 0;
        int r = editor_vim_key_logical(27);
        if (r != EDITOR_CONTINUE) {
            return r;
        }
        return editor_vim_key_logical(byte);
    }
    if (es == 2) {
        if (byte >= 0x40 && byte <= 0x7E) {
            es = 0;
            int lk = -1;
            switch (byte) {
            case 'A':
                lk = KEY_UP;
                break;
            case 'B':
                lk = KEY_DOWN;
                break;
            case 'C':
                lk = KEY_RIGHT;
                break;
            case 'D':
                lk = KEY_LEFT;
                break;
            case 'H':
                lk = KEY_HOME;
                break;
            case 'F':
                lk = KEY_END;
                break;
            case '~':
                if (csi_len > 0 && csi[0] == '3') {
                    lk = KEY_DEL;
                } else if (csi_len > 0 && csi[0] == '5') {
                    lk = KEY_PGUP;
                } else if (csi_len > 0 && csi[0] == '6') {
                    lk = KEY_PGDN;
                } else if (csi_len > 0 && (csi[0] == '1' || csi[0] == '7')) {
                    lk = KEY_HOME;
                } else if (csi_len > 0 && (csi[0] == '4' || csi[0] == '8')) {
                    lk = KEY_END;
                }
                break;
            default:
                break;
            }
            return lk >= 0 ? editor_vim_key_logical(lk) : EDITOR_CONTINUE;
        }
        if (csi_len < (int)sizeof(csi) - 1) {
            csi[csi_len++] = (char)byte;
        }
        return EDITOR_CONTINUE;
    }
    if (byte == 27) {
        es = 1;
        return EDITOR_CONTINUE;
    }
    // Normalize the control bytes the handlers expect as logical keys.
    if (byte == '\r' || byte == '\n') {
        return editor_vim_key_logical(KEY_ENTER);
    }
    if (byte == 0x7f || byte == 0x08) {
        return editor_vim_key_logical(KEY_BACKSPACE);
    }
    return editor_vim_key_logical(byte);
}

void editor_vim_open(const char* path)
{
    load_file(path);
    cx = cy = rowoff = coloff = 0;
    dirty = false;
    status_msg = NULL;
    vmode = M_NORMAL;
    vcount = 0;
    vpending = 0;
    vcmd_len = 0;
    for (int i = 0; i < VUNDO_MAX; i++) {
        if (vundo[i]) {
            heap_free(heap_default(), vundo[i]);
            vundo[i] = NULL;
        }
    }
    vundo_head = vundo_count = 0;
    if (vyank) {
        heap_free(heap_default(), vyank);
        vyank = NULL;
        vyank_len = 0;
    }
}

// --- windowed rendering -----------------------------------------------------

static mu_Color vrgb(uint32_t c)
{
    return mu_color((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, 255);
}

void editor_vim_draw(mu_Context* ctx)
{
    mu_Container* cnt = mu_get_current_container(ctx);
    if (cnt == NULL) {
        return;
    }
    mu_Rect b = cnt->body;
    mu_draw_rect(ctx, b, vrgb(0x0e1116)); // dark editor background

    int cols = b.w / VGW;
    int rows = b.h / VGH;
    if (cols < 1) {
        cols = 1;
    }
    if (rows < 2) {
        rows = 2;
    }
    term_cols = (size_t)cols;
    term_rows = (size_t)rows;
    scroll_to_cursor();

    int textrows = rows - 1;
    size_t width = view_width();
    static char colored[ED_MAX_COLS * 12];
    for (int y = 0; y < textrows; y++) {
        size_t row = rowoff + (size_t)y;
        if (row >= nlines) {
            break;
        }
        size_t len = line_len[row];
        if (coloff < len) {
            size_t vis = len - coloff;
            if (vis > width) {
                vis = width;
            }
            highlight_lua(line_buf[row] + coloff, vis, colored, sizeof colored);
            ui_text_ansi(ctx, colored, b.x, b.y + y * VGH);
        }
    }

    // Cursor: block in NORMAL, thin bar in INSERT.
    int cxs = (int)(cx - coloff);
    int cys = (int)(cy - rowoff);
    int px = b.x + cxs * VGW;
    int py = b.y + cys * VGH;
    if (vmode == M_INSERT) {
        mu_draw_rect(ctx, mu_rect(px, py, 2, VGH), vrgb(0x9ecbff));
    } else if (vmode == M_NORMAL) {
        mu_draw_rect(ctx, mu_rect(px, py, VGW, VGH), vrgb(0x9ecbff));
        if (cy < nlines && cx < line_len[cy]) {
            char ch[1] = {line_buf[cy][cx]};
            mu_draw_text(ctx, NULL, ch, 1, mu_vec2(px, py), vrgb(0x0e1116));
        }
    }

    // Status bar.
    int sy = b.y + textrows * VGH;
    mu_draw_rect(ctx, mu_rect(b.x, sy, b.w, VGH), vrgb(0x24406a));
    char st[256];
    if (vmode == M_COMMAND) {
        snprintf(st, sizeof st, ":%.*s", (int)vcmd_len, vcmd);
    } else if (vmode == M_SEARCH) {
        snprintf(st, sizeof st, "/%.*s", (int)vcmd_len, vcmd);
    } else {
        const char* mn = vmode == M_INSERT ? "-- INSERT --" : "-- NORMAL --";
        snprintf(st, sizeof st, "%s  %s%s   Ln %u, Col %u%s%s", mn, filepath,
                 dirty ? " *" : "", (unsigned)(cy + 1), (unsigned)(cx + 1),
                 status_msg ? "   " : "", status_msg ? status_msg : "");
    }
    size_t stn = 0;
    while (st[stn] && (int)stn < cols) {
        stn++;
    }
    mu_draw_text(ctx, NULL, st, (int)stn, mu_vec2(b.x, sy), vrgb(0xffffff));
}
