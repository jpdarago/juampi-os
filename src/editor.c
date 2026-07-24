// Full-screen Lua editor (see editor.h). A line-array buffer, redrawn each
// keystroke into one ANSI frame so the framebuffer terminal and a serial
// terminal render identically. Each visible line is colorized with the same
// highlight_lua() the REPL line editor uses.

#include <editor.h>
#include <console.h>
#include <highlight.h>
#include <ext2.h>
#include <memory.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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

// Load `path` into the line array; a missing file yields a single empty line.
static void load_file(const char* path)
{
    // Copy the path (bounded) for the status bar and for saving.
    size_t i = 0;
    for (; path[i] != '\0' && i < sizeof(filepath) - 1; i++) {
        filepath[i] = path[i];
    }
    filepath[i] = '\0';

    nlines = 0;
    size_t size = 0;
    void* data = ext2_read_path(path, &size);
    if (data == NULL) {
        nlines = 1;
        line_len[0] = 0;
        line_buf[0][0] = '\0';
        return;
    }

    const char* p = (const char*)data;
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
