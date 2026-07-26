// Text editor (see editor.h), as a reentrant instance: every piece of state —
// the document, cursor, vim mode, undo ring, yank register, render scratch,
// even the escape-sequence decoder — lives in a `struct editor` allocated from
// the arena the caller hands editor_open(). There are no file-scope mutable
// statics, so multiple editors can exist at once, and an instance may be owned
// by any single core: the only shared touchpoints are the console, ext2, and
// gfx services, which remain the machine-wide serialization boundary (BSP-only
// today, per docs/ui.md).
//
// Two frontends over one buffer engine:
//   * editor_run()      — the classic full-screen ANSI editor
//   (headless/serial).
//   * editor_vim_key()/editor_vim_draw() — the windowed vim-style editor,
//   driven
//     frame-by-frame by ui_edit() in src/ui.c.
//
// Allocation discipline: all buffers come from the instance's arena (`e->mem`),
// sized so the whole lifetime fits (see EDITOR_ARENA_SIZE in ui.c). Bounded
// structures that would churn (undo snapshots, the yank register) are fixed-
// capacity slots allocated once and reused. The blob ext2_read_path() returns
// is owned by the fs layer, so it is released with ext2_free() after parsing.

#include <editor.h>
#include <console.h>
#include <highlight.h>
#include <ext2.h>
#include <ui.h> // windowed vim editor: ui_text_ansi

#include <printf/printf.h> // snprintf for the status bar
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "microui/microui.h"

#define ED_MAX_LINES 1024
#define ED_MAX_COLS 256 // characters per line (long lines are clipped on load)

// Render scratch: one ANSI frame for the classic editor (also reused as the
// save-serialization buffer), and one highlighted line.
#define ED_SCR_MAX (512 * 1024)
#define ED_HL_MAX (ED_MAX_COLS * 12)

// Undo: a ring of fixed-capacity serialized snapshots. Slots are allocated from
// the arena on first use and reused forever — no frees, no churn. A buffer that
// serializes past the cap simply isn't snapshotted (reported in the status
// bar).
#define ED_UNDO_MAX 16
#define ED_UNDO_CAP (64 * 1024)

// Linewise yank register, same fixed-slot treatment.
#define ED_YANK_CAP (64 * 1024)

enum { M_NORMAL, M_INSERT, M_COMMAND, M_SEARCH };

// One document line (fixed capacity); the buffer is an arena array of these.
typedef char ed_line[ED_MAX_COLS + 1];

struct editor {
    allocator* mem; // the widget's lifetime allocator (an arena)

    // Document: nlines lines, each a NUL-terminated string of line_len[i]
    // chars.
    ed_line* lines;
    size_t* line_len;
    size_t nlines;

    // Cursor and view.
    size_t cx, cy;         // cursor position within the buffer (col, row)
    size_t rowoff, coloff; // top-left visible cell
    bool dirty;            // unsaved changes
    char filepath[128];
    const char* status_msg;      // transient message shown in the status bar
    size_t term_cols, term_rows; // refreshed each frame

    // Scratch buffers (arena; scr doubles as the save-serialization buffer).
    char* scr; // ED_SCR_MAX, allocated on first use
    size_t scr_len;
    char* hl; // ED_HL_MAX + 1, per-line highlight output

    // Vim modal state.
    int vmode;
    int vcount;     // numeric count prefix being typed (0 = none)
    int vpending;   // pending operator: 'd','c','g','r','y' (0 = none)
    char vcmd[128]; // the ':' command / '/' search being typed
    size_t vcmd_len;
    char vsearch[128]; // last committed search pattern
    size_t vsearch_len;

    // Yank register (linewise, fixed capacity, allocated on first use).
    char* yank;
    size_t yank_len;

    // Undo ring of fixed-capacity slots (allocated on first use, reused).
    char* undo[ED_UNDO_MAX];
    size_t undo_len[ED_UNDO_MAX];
    size_t undo_cx[ED_UNDO_MAX], undo_cy[ED_UNDO_MAX];
    int undo_head, undo_count;

    // Byte-level VT100 decoder state for editor_vim_key.
    int key_es;
    char key_csi[8];
    int key_csi_len;
};

// --- One-frame output buffer (classic renderer) ------------------------------

// Ensure the frame/serialization scratch exists (first classic render or save).
static char* ed_scratch(editor* e)
{
    if (e->scr == NULL) {
        e->scr = new (e->mem, char, ED_SCR_MAX);
    }
    return e->scr;
}

static void s_raw(editor* e, const char* p, size_t n)
{
    if (e->scr_len + n > ED_SCR_MAX) {
        return; // frame buffer full; drop the tail (never happens in practice)
    }
    for (size_t i = 0; i < n; i++) {
        e->scr[e->scr_len++] = p[i];
    }
}

static void s_puts(editor* e, const char* z)
{
    size_t n = 0;
    while (z[n]) {
        n++;
    }
    s_raw(e, z, n);
}

// Append an unsigned decimal (used for the CSI coordinates).
static void s_putu(editor* e, size_t v)
{
    char tmp[20];
    size_t i = 0;
    if (v == 0) {
        s_raw(e, "0", 1);
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
    s_raw(e, out, i);
}

// Move the cursor to (row, col), 1-based (ANSI CUP).
static void s_moveto(editor* e, size_t row, size_t col)
{
    s_puts(e, "\033[");
    s_putu(e, row);
    s_puts(e, ";");
    s_putu(e, col);
    s_puts(e, "H");
}

// --- Buffer helpers ---------------------------------------------------------

static void set_status(editor* e, const char* msg)
{
    e->status_msg = msg;
}

// Parse `size` bytes of text `p` into the line array (splitting on '\n',
// dropping '\r', clipping long lines). Shared by load_file and undo.
static void buffer_from_text(editor* e, const char* p, size_t size)
{
    e->nlines = 0;
    size_t col = 0;
    e->line_len[0] = 0;
    for (size_t j = 0; j < size && e->nlines < ED_MAX_LINES; j++) {
        char ch = p[j];
        if (ch == '\n') {
            e->lines[e->nlines][col] = '\0';
            e->line_len[e->nlines] = col;
            e->nlines++;
            col = 0;
            if (e->nlines < ED_MAX_LINES) {
                e->lines[e->nlines][0] = '\0';
            }
        } else if (ch == '\r') {
            continue; // drop CR from CRLF files
        } else if (col < ED_MAX_COLS) {
            e->lines[e->nlines][col++] = ch;
        }
        // Characters past ED_MAX_COLS are clipped.
    }
    // Close the final line unless the file ended exactly on a newline.
    if ((col > 0 || e->nlines == 0) && e->nlines < ED_MAX_LINES) {
        e->lines[e->nlines][col] = '\0';
        e->line_len[e->nlines] = col;
        e->nlines++;
    }
    if (e->nlines == 0) {
        e->nlines = 1;
        e->line_len[0] = 0;
        e->lines[0][0] = '\0';
    }
}

// Load `path` into the line array; a missing file yields a single empty line.
static void load_file(editor* e, const char* path)
{
    // Copy the path (bounded) for the status bar and for saving.
    size_t i = 0;
    for (; path[i] != '\0' && i < sizeof(e->filepath) - 1; i++) {
        e->filepath[i] = path[i];
    }
    e->filepath[i] = '\0';

    size_t size = 0;
    void* data = ext2_read_path(path, &size);
    if (data == NULL) {
        e->nlines = 1;
        e->line_len[0] = 0;
        e->lines[0][0] = '\0';
        return;
    }
    buffer_from_text(e, (const char*)data, size);
    // The blob is the fs layer's (default-heap) allocation, not widget memory.
    ext2_free(data);
}

// Serialize the buffer (lines joined with '\n') into `out` (capacity `cap`).
// Returns the length, or 0 if it does not fit.
static size_t serialize_into(editor* e, char* out, size_t cap)
{
    size_t total = 0;
    for (size_t i = 0; i < e->nlines; i++) {
        total += e->line_len[i] + 1; // + '\n'
    }
    if (total > cap) {
        return 0;
    }
    size_t o = 0;
    for (size_t i = 0; i < e->nlines; i++) {
        for (size_t j = 0; j < e->line_len[i]; j++) {
            out[o++] = e->lines[i][j];
        }
        out[o++] = '\n';
    }
    return o;
}

// Serialize into the frame scratch and write back to ext2.
static bool save_file(editor* e)
{
    char* out = ed_scratch(e);
    size_t total = serialize_into(e, out, ED_SCR_MAX);
    if (total == 0) {
        return false;
    }
    bool ok = ext2_write_file(e->filepath, out, total);
    if (ok) {
        e->dirty = false;
    }
    return ok;
}

// Insert a printable character at the cursor.
static void insert_char(editor* e, char ch)
{
    if (e->line_len[e->cy] >= ED_MAX_COLS) {
        return; // line full
    }
    char* line = e->lines[e->cy];
    for (size_t i = e->line_len[e->cy]; i > e->cx; i--) {
        line[i] = line[i - 1];
    }
    line[e->cx] = ch;
    e->line_len[e->cy]++;
    line[e->line_len[e->cy]] = '\0';
    e->cx++;
    e->dirty = true;
}

// Split the current line at the cursor, pushing the tail down into a new line.
static void insert_newline(editor* e)
{
    if (e->nlines >= ED_MAX_LINES) {
        return;
    }
    // Make room for one more line after cy.
    for (size_t i = e->nlines; i > e->cy + 1; i--) {
        for (size_t j = 0; j <= e->line_len[i - 1]; j++) {
            e->lines[i][j] = e->lines[i - 1][j];
        }
        e->line_len[i] = e->line_len[i - 1];
    }
    e->nlines++;

    // Move the tail [cx, len) of the current line into the new line cy+1.
    size_t tail = e->line_len[e->cy] - e->cx;
    for (size_t j = 0; j < tail; j++) {
        e->lines[e->cy + 1][j] = e->lines[e->cy][e->cx + j];
    }
    e->lines[e->cy + 1][tail] = '\0';
    e->line_len[e->cy + 1] = tail;

    e->lines[e->cy][e->cx] = '\0';
    e->line_len[e->cy] = e->cx;

    e->cy++;
    e->cx = 0;
    e->dirty = true;
}

// Append line `src` onto the end of line `dst` (bounded), used when joining.
static void join_into(editor* e, size_t dst, size_t src)
{
    size_t room = ED_MAX_COLS - e->line_len[dst];
    size_t n = e->line_len[src];
    if (n > room) {
        n = room;
    }
    for (size_t j = 0; j < n; j++) {
        e->lines[dst][e->line_len[dst] + j] = e->lines[src][j];
    }
    e->line_len[dst] += n;
    e->lines[dst][e->line_len[dst]] = '\0';
}

// Remove line `row` by shifting the lines below it up.
static void delete_line(editor* e, size_t row)
{
    for (size_t i = row; i + 1 < e->nlines; i++) {
        for (size_t j = 0; j <= e->line_len[i + 1]; j++) {
            e->lines[i][j] = e->lines[i + 1][j];
        }
        e->line_len[i] = e->line_len[i + 1];
    }
    e->nlines--;
}

// Delete the character before the cursor, joining with the previous line at
// BOL.
static void do_backspace(editor* e)
{
    if (e->cx > 0) {
        char* line = e->lines[e->cy];
        for (size_t i = e->cx - 1; i + 1 < e->line_len[e->cy]; i++) {
            line[i] = line[i + 1];
        }
        e->line_len[e->cy]--;
        line[e->line_len[e->cy]] = '\0';
        e->cx--;
        e->dirty = true;
    } else if (e->cy > 0) {
        size_t prev_len = e->line_len[e->cy - 1];
        join_into(e, e->cy - 1, e->cy);
        delete_line(e, e->cy);
        e->cy--;
        e->cx = prev_len;
        e->dirty = true;
    }
}

// Delete the character at the cursor, joining the next line at EOL.
static void do_delete(editor* e)
{
    if (e->cx < e->line_len[e->cy]) {
        char* line = e->lines[e->cy];
        for (size_t i = e->cx; i + 1 < e->line_len[e->cy]; i++) {
            line[i] = line[i + 1];
        }
        e->line_len[e->cy]--;
        line[e->line_len[e->cy]] = '\0';
        e->dirty = true;
    } else if (e->cy + 1 < e->nlines) {
        join_into(e, e->cy, e->cy + 1);
        delete_line(e, e->cy + 1);
        e->dirty = true;
    }
}

// Clamp the cursor column to the current line's length (after vertical motion).
static void clamp_cx(editor* e)
{
    if (e->cx > e->line_len[e->cy]) {
        e->cx = e->line_len[e->cy];
    }
}

// --- Rendering (classic full-screen ANSI) ------------------------------------

// Effective editable width, capped so a very wide terminal can't overrun the
// per-line color scratch buffer.
static size_t view_width(editor* e)
{
    return e->term_cols > ED_MAX_COLS ? ED_MAX_COLS : e->term_cols;
}

// Adjust the scroll offsets so the cursor is on screen.
static void scroll_to_cursor(editor* e)
{
    size_t textrows = e->term_rows - 1;
    size_t width = view_width(e);
    if (e->cy < e->rowoff) {
        e->rowoff = e->cy;
    }
    if (e->cy >= e->rowoff + textrows) {
        e->rowoff = e->cy - textrows + 1;
    }
    if (e->cx < e->coloff) {
        e->coloff = e->cx;
    }
    if (e->cx >= e->coloff + width) {
        e->coloff = e->cx - width + 1;
    }
}

static void render_line(editor* e, size_t row)
{
    size_t len = e->line_len[row];
    size_t width = view_width(e);
    if (e->coloff >= len) {
        return; // scrolled entirely past this (short) line
    }
    size_t vis = len - e->coloff;
    if (vis > width) {
        vis = width;
    }
    highlight_lua(e->lines[row] + e->coloff, vis, e->hl, ED_HL_MAX);
    s_puts(e, e->hl);
}

static void draw_status(editor* e)
{
    s_moveto(e, e->term_rows, 1);
    s_puts(e, "\033[7m"); // reverse video
    size_t used = 0;

    s_raw(e, " ", 1);
    used++;
    for (size_t i = 0; e->filepath[i] && used < e->term_cols; i++, used++) {
        s_raw(e, &e->filepath[i], 1);
    }
    if (e->dirty && used + 2 < e->term_cols) {
        s_puts(e, " *");
        used += 2;
    }

    // Right-aligned "Ln x, Col y" plus a transient message / key hints.
    const char* right =
            e->status_msg ? e->status_msg : "^S save  ^X run  ^Q quit";
    char pos[64];
    snprintf(pos, sizeof pos, "  Ln %u, Col %u", (unsigned)(e->cy + 1),
             (unsigned)(e->cx + 1));
    size_t k = 0;
    while (pos[k]) {
        k++;
    }

    // Right segment = pos + "  " + right; pad the middle with spaces so it sits
    // flush against the right edge (truncated if the terminal is narrow).
    size_t rlen = k + 2;
    for (const char* q = right; *q; q++) {
        rlen++;
    }
    while (used + rlen < e->term_cols) {
        s_raw(e, " ", 1);
        used++;
    }
    if (used < e->term_cols) {
        s_puts(e, pos);
        s_puts(e, "  ");
        s_puts(e, right);
        used += rlen;
    }
    while (used < e->term_cols) {
        s_raw(e, " ", 1);
        used++;
    }
    s_puts(e, "\033[0m");
}

static void render(editor* e)
{
    console_dimensions(&e->term_cols, &e->term_rows);
    if (e->term_rows < 2) {
        e->term_rows = 2;
    }
    scroll_to_cursor(e);

    ed_scratch(e);
    e->scr_len = 0;
    s_puts(e, "\033[?25l"); // hide cursor while composing
    s_puts(e, "\033[H");

    size_t textrows = e->term_rows - 1;
    for (size_t y = 0; y < textrows; y++) {
        size_t row = e->rowoff + y;
        s_moveto(e, y + 1, 1);
        if (row < e->nlines) {
            render_line(e, row);
        }
        s_puts(e, "\033[K"); // clear the rest of the row
    }

    draw_status(e);

    // Place the hardware cursor at the edit position and reveal it.
    s_moveto(e, (e->cy - e->rowoff) + 1, (e->cx - e->coloff) + 1);
    s_puts(e, "\033[?25h");

    console_write(e->scr, e->scr_len);
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
// into `params`, final byte into *final_out. (Blocking; classic editor only.)
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

// --- Classic full-screen editor (headless fallback) --------------------------

int editor_run(editor* e)
{
    bool quit_armed = false; // set after a Ctrl-Q on a dirty buffer

    for (;;) {
        render(e);
        int k = read_key();

        // Any key other than a repeated Ctrl-Q cancels the pending-quit
        // warning.
        if (k != CTRL('q')) {
            quit_armed = false;
            if (e->status_msg && k != -1) {
                e->status_msg = NULL;
            }
        }

        switch (k) {
        case CTRL('q'):
            if (e->dirty && !quit_armed) {
                set_status(e, "Unsaved changes - Ctrl-Q again to quit");
                quit_armed = true;
                break;
            }
            console_clear();
            return EDITOR_QUIT;
        case CTRL('s'):
            set_status(e, save_file(e) ? "Saved" : "Save failed");
            break;
        case CTRL('x'):
            if (save_file(e)) {
                console_clear();
                return EDITOR_RUN;
            }
            set_status(e, "Save failed");
            break;
        case KEY_UP:
            if (e->cy > 0) {
                e->cy--;
                clamp_cx(e);
            }
            break;
        case KEY_DOWN:
            if (e->cy + 1 < e->nlines) {
                e->cy++;
                clamp_cx(e);
            }
            break;
        case KEY_LEFT:
            if (e->cx > 0) {
                e->cx--;
            } else if (e->cy > 0) {
                e->cy--;
                e->cx = e->line_len[e->cy];
            }
            break;
        case KEY_RIGHT:
            if (e->cx < e->line_len[e->cy]) {
                e->cx++;
            } else if (e->cy + 1 < e->nlines) {
                e->cy++;
                e->cx = 0;
            }
            break;
        case KEY_HOME:
        case CTRL('a'):
            e->cx = 0;
            break;
        case KEY_END:
        case CTRL('e'):
            e->cx = e->line_len[e->cy];
            break;
        case KEY_PGUP: {
            size_t page = e->term_rows - 1;
            e->cy = e->cy > page ? e->cy - page : 0;
            clamp_cx(e);
            break;
        }
        case KEY_PGDN: {
            size_t page = e->term_rows - 1;
            e->cy = e->cy + page < e->nlines ? e->cy + page : e->nlines - 1;
            clamp_cx(e);
            break;
        }
        case KEY_ENTER:
            insert_newline(e);
            break;
        case KEY_BACKSPACE:
            do_backspace(e);
            break;
        case KEY_DEL:
            do_delete(e);
            break;
        default:
            if (k >= 0x20 && k < 0x7F) {
                insert_char(e, (char)k);
            }
            break;
        }
    }
}

// ============================================================================
// Vim-style modal layer for the windowed editor (ui_edit in ui.c).
// ============================================================================

#define VGW 8
#define VGH 16

static bool ed_isword(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}
static bool ed_isspace(char c)
{
    return c == ' ' || c == '\t';
}

// Snapshot the buffer for undo before a modifying command group. Slots are
// fixed-capacity and reused; an oversized buffer is simply not snapshotted.
static void push_undo(editor* e)
{
    int slot = e->undo_head;
    if (e->undo[slot] == NULL) {
        e->undo[slot] = new (e->mem, char, ED_UNDO_CAP);
    }
    size_t len = serialize_into(e, e->undo[slot], ED_UNDO_CAP);
    if (len == 0) {
        set_status(e, "Change too large for undo");
        return;
    }
    e->undo_len[slot] = len;
    e->undo_cx[slot] = e->cx;
    e->undo_cy[slot] = e->cy;
    e->undo_head = (e->undo_head + 1) % ED_UNDO_MAX;
    if (e->undo_count < ED_UNDO_MAX) {
        e->undo_count++;
    }
}

static void do_undo(editor* e)
{
    if (e->undo_count == 0) {
        set_status(e, "Already at oldest change");
        return;
    }
    e->undo_head = (e->undo_head - 1 + ED_UNDO_MAX) % ED_UNDO_MAX;
    e->undo_count--;
    int slot = e->undo_head;
    buffer_from_text(e, e->undo[slot], e->undo_len[slot]);
    e->cx = e->undo_cx[slot];
    e->cy = e->undo_cy[slot];
    if (e->cy >= e->nlines) {
        e->cy = e->nlines ? e->nlines - 1 : 0;
    }
    clamp_cx(e);
    e->dirty = true;
}

// --- motions ----------------------------------------------------------------

static void vim_w(editor* e)
{
    size_t len = e->line_len[e->cy];
    if (e->cx < len && !ed_isspace(e->lines[e->cy][e->cx])) {
        bool word = ed_isword(e->lines[e->cy][e->cx]);
        while (e->cx < len && !ed_isspace(e->lines[e->cy][e->cx]) &&
               ed_isword(e->lines[e->cy][e->cx]) == word) {
            e->cx++;
        }
    }
    for (;;) {
        len = e->line_len[e->cy];
        if (e->cx >= len) {
            if (e->cy + 1 < e->nlines) {
                e->cy++;
                e->cx = 0;
            } else {
                e->cx = len;
                return;
            }
        } else if (ed_isspace(e->lines[e->cy][e->cx])) {
            e->cx++;
        } else {
            return;
        }
    }
}

static void vim_b(editor* e)
{
    if (e->cx == 0) {
        if (e->cy == 0) {
            return;
        }
        e->cy--;
        e->cx = e->line_len[e->cy];
    } else {
        e->cx--;
    }
    while (e->cx > 0 && (e->cx >= e->line_len[e->cy] ||
                         ed_isspace(e->lines[e->cy][e->cx]))) {
        e->cx--;
    }
    if (e->cx < e->line_len[e->cy] && !ed_isspace(e->lines[e->cy][e->cx])) {
        bool word = ed_isword(e->lines[e->cy][e->cx]);
        while (e->cx > 0 && !ed_isspace(e->lines[e->cy][e->cx - 1]) &&
               ed_isword(e->lines[e->cy][e->cx - 1]) == word) {
            e->cx--;
        }
    }
}

static void vim_e(editor* e)
{
    size_t len = e->line_len[e->cy];
    if (e->cx + 1 >= len) {
        if (e->cy + 1 < e->nlines) {
            e->cy++;
            e->cx = 0;
        }
    } else {
        e->cx++;
    }
    for (;;) {
        len = e->line_len[e->cy];
        if (e->cx >= len) {
            if (e->cy + 1 < e->nlines) {
                e->cy++;
                e->cx = 0;
            } else {
                return;
            }
        } else if (ed_isspace(e->lines[e->cy][e->cx])) {
            e->cx++;
        } else {
            break;
        }
    }
    len = e->line_len[e->cy];
    if (e->cx < len && !ed_isspace(e->lines[e->cy][e->cx])) {
        bool word = ed_isword(e->lines[e->cy][e->cx]);
        while (e->cx + 1 < len && !ed_isspace(e->lines[e->cy][e->cx + 1]) &&
               ed_isword(e->lines[e->cy][e->cx + 1]) == word) {
            e->cx++;
        }
    }
}

// --- edits ------------------------------------------------------------------

// Delete the char at the cursor without joining lines (vim 'x').
static void del_char(editor* e)
{
    if (e->cx >= e->line_len[e->cy]) {
        return;
    }
    char* line = e->lines[e->cy];
    for (size_t i = e->cx; i + 1 < e->line_len[e->cy]; i++) {
        line[i] = line[i + 1];
    }
    e->line_len[e->cy]--;
    line[e->line_len[e->cy]] = '\0';
    e->dirty = true;
}

// Delete from the cursor to the start of the next word, on the current line
// (vim 'dw').
static void del_word(editor* e)
{
    size_t len = e->line_len[e->cy];
    size_t end = e->cx;
    if (end < len) {
        if (ed_isspace(e->lines[e->cy][end])) {
            while (end < len && ed_isspace(e->lines[e->cy][end])) {
                end++;
            }
        } else {
            bool word = ed_isword(e->lines[e->cy][end]);
            while (end < len && !ed_isspace(e->lines[e->cy][end]) &&
                   ed_isword(e->lines[e->cy][end]) == word) {
                end++;
            }
            while (end < len && ed_isspace(e->lines[e->cy][end])) {
                end++;
            }
        }
    }
    char* line = e->lines[e->cy];
    for (size_t i = end; i <= e->line_len[e->cy]; i++) {
        line[e->cx + (i - end)] = line[i];
    }
    e->line_len[e->cy] -= (end - e->cx);
    e->dirty = true;
}

static void vim_join(editor* e)
{
    if (e->cy + 1 >= e->nlines) {
        return;
    }
    size_t at = e->line_len[e->cy];
    size_t s = 0;
    while (s < e->line_len[e->cy + 1] && ed_isspace(e->lines[e->cy + 1][s])) {
        s++;
    }
    if (e->line_len[e->cy] > 0 && s < e->line_len[e->cy + 1] &&
        at < ED_MAX_COLS) {
        e->lines[e->cy][at++] = ' ';
    }
    for (size_t j = s; j < e->line_len[e->cy + 1] && at < ED_MAX_COLS; j++) {
        e->lines[e->cy][at++] = e->lines[e->cy + 1][j];
    }
    e->lines[e->cy][at] = '\0';
    e->line_len[e->cy] = at;
    delete_line(e, e->cy + 1);
    e->dirty = true;
}

// Copy `count` lines starting at `from` into the yank register (linewise,
// truncated at the register's fixed capacity).
static void yank_lines(editor* e, size_t from, size_t count)
{
    if (e->yank == NULL) {
        e->yank = new (e->mem, char, ED_YANK_CAP);
    }
    size_t o = 0;
    for (size_t i = 0; i < count && from + i < e->nlines; i++) {
        size_t need = e->line_len[from + i] + 1;
        if (o + need > ED_YANK_CAP) {
            set_status(e, "Yank truncated");
            break;
        }
        for (size_t j = 0; j < e->line_len[from + i]; j++) {
            e->yank[o++] = e->lines[from + i][j];
        }
        e->yank[o++] = '\n';
    }
    e->yank_len = o;
}

static void insert_line_at(editor* e, size_t at, const char* text, size_t len)
{
    if (e->nlines >= ED_MAX_LINES) {
        return;
    }
    for (size_t i = e->nlines; i > at; i--) {
        for (size_t j = 0; j <= e->line_len[i - 1]; j++) {
            e->lines[i][j] = e->lines[i - 1][j];
        }
        e->line_len[i] = e->line_len[i - 1];
    }
    size_t n = len > ED_MAX_COLS ? ED_MAX_COLS : len;
    for (size_t j = 0; j < n; j++) {
        e->lines[at][j] = text[j];
    }
    e->lines[at][n] = '\0';
    e->line_len[at] = n;
    e->nlines++;
}

// Paste the (linewise) yank register below or above the current line.
static void paste_lines(editor* e, bool below)
{
    if (e->yank == NULL || e->yank_len == 0) {
        return;
    }
    size_t at = below ? e->cy + 1 : e->cy;
    size_t first = at;
    size_t start = 0;
    for (size_t i = 0; i < e->yank_len; i++) {
        if (e->yank[i] == '\n') {
            insert_line_at(e, at, e->yank + start, i - start);
            at++;
            start = i + 1;
        }
    }
    e->cy = first;
    e->cx = 0;
    e->dirty = true;
}

// Linear substring search from just after the cursor (dir +1) or before it
// (dir -1), wrapping. Moves the cursor to a match or reports not found.
static void do_search(editor* e, int dir)
{
    if (e->vsearch_len == 0) {
        return;
    }
    for (size_t step = 1; step <= e->nlines; step++) {
        size_t row = (dir > 0) ? (e->cy + step) % e->nlines
                               : (e->cy + e->nlines - step) % e->nlines;
        const char* line = e->lines[row];
        size_t len = e->line_len[row];
        for (size_t c = 0; c + e->vsearch_len <= len; c++) {
            size_t m = 0;
            while (m < e->vsearch_len && line[c + m] == e->vsearch[m]) {
                m++;
            }
            if (m == e->vsearch_len) {
                e->cy = row;
                e->cx = c;
                return;
            }
        }
    }
    set_status(e, "Pattern not found");
}

// --- dispatch ---------------------------------------------------------------

static int run_command(editor* e)
{
    e->vcmd[e->vcmd_len] = '\0';
    const char* c = e->vcmd;
    // A bare number jumps to that line.
    if (c[0] >= '1' && c[0] <= '9') {
        size_t n = 0;
        for (size_t i = 0; c[i] >= '0' && c[i] <= '9'; i++) {
            n = n * 10 + (size_t)(c[i] - '0');
        }
        if (n >= 1 && n <= e->nlines) {
            e->cy = n - 1;
            clamp_cx(e);
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
    if (e->vcmd_len >= 3 && c[0] == 'r' && c[1] == 'u' && c[2] == 'n') {
        run = true;
        w = true;
    }
    if (w) {
        set_status(e, save_file(e) ? "Saved" : "Save failed");
        if (!e->dirty && run) {
            return EDITOR_RUN;
        }
    }
    if (q) {
        if (e->dirty && !force && !w) {
            set_status(e, "No write since last change (add ! to override)");
            return EDITOR_CONTINUE;
        }
        return EDITOR_QUIT;
    }
    if (!w && !run) {
        set_status(e, "Unknown command");
    }
    return EDITOR_CONTINUE;
}

static int handle_normal(editor* e, int k)
{
    // Numeric count prefix ('0' is a motion unless a count is in progress).
    if ((k >= '1' && k <= '9') || (k == '0' && e->vcount > 0)) {
        e->vcount = e->vcount * 10 + (k - '0');
        return EDITOR_CONTINUE;
    }
    int cnt = e->vcount > 0 ? e->vcount : 1;

    // Pending operators.
    if (e->vpending) {
        int op = e->vpending;
        e->vpending = 0;
        e->vcount = 0;
        if (op == 'r') {
            if (k >= 0x20 && k < 0x7F && e->cx < e->line_len[e->cy]) {
                push_undo(e);
                e->lines[e->cy][e->cx] = (char)k;
                e->dirty = true;
            }
        } else if (op == 'g') {
            if (k == 'g') {
                e->cy = 0;
                clamp_cx(e);
            }
        } else if (op == 'd') {
            if (k == 'd') {
                push_undo(e);
                yank_lines(e, e->cy, (size_t)cnt);
                if (e->nlines <= 1) {
                    // Deleting the only line leaves one empty line (like vim).
                    e->line_len[0] = 0;
                    e->lines[0][0] = '\0';
                    e->cx = 0;
                } else {
                    for (int i = 0; i < cnt && e->nlines > 1; i++) {
                        delete_line(e, e->cy);
                    }
                    if (e->cy >= e->nlines) {
                        e->cy = e->nlines - 1;
                    }
                }
                clamp_cx(e);
            } else if (k == 'w') {
                push_undo(e);
                del_word(e);
            }
        } else if (op == 'c') {
            if (k == 'c') {
                push_undo(e);
                e->line_len[e->cy] = 0;
                e->lines[e->cy][0] = '\0';
                e->cx = 0;
                e->vmode = M_INSERT;
            } else if (k == 'w') {
                push_undo(e);
                del_word(e);
                e->vmode = M_INSERT;
            }
        } else if (op == 'y') {
            if (k == 'y') {
                yank_lines(e, e->cy, (size_t)cnt);
                set_status(e, "Yanked");
            }
        }
        return EDITOR_CONTINUE;
    }

    e->vcount = 0;
    switch (k) {
    case 'h':
    case KEY_LEFT:
        for (int i = 0; i < cnt && e->cx > 0; i++) {
            e->cx--;
        }
        break;
    case 'l':
    case KEY_RIGHT:
        for (int i = 0; i < cnt && e->cx < e->line_len[e->cy]; i++) {
            e->cx++;
        }
        break;
    case 'j':
    case KEY_DOWN:
        for (int i = 0; i < cnt && e->cy + 1 < e->nlines; i++) {
            e->cy++;
        }
        clamp_cx(e);
        break;
    case 'k':
    case KEY_UP:
        for (int i = 0; i < cnt && e->cy > 0; i++) {
            e->cy--;
        }
        clamp_cx(e);
        break;
    case '0':
    case KEY_HOME:
        e->cx = 0;
        break;
    case '^': {
        size_t i = 0;
        while (i < e->line_len[e->cy] && ed_isspace(e->lines[e->cy][i])) {
            i++;
        }
        e->cx = i;
        break;
    }
    case '$':
    case KEY_END:
        e->cx = e->line_len[e->cy];
        break;
    case 'w':
        for (int i = 0; i < cnt; i++) {
            vim_w(e);
        }
        break;
    case 'b':
        for (int i = 0; i < cnt; i++) {
            vim_b(e);
        }
        break;
    case 'e':
        for (int i = 0; i < cnt; i++) {
            vim_e(e);
        }
        break;
    case 'G':
        e->cy = (cnt > 1) ? (size_t)cnt - 1 : e->nlines - 1;
        if (e->cy >= e->nlines) {
            e->cy = e->nlines - 1;
        }
        clamp_cx(e);
        break;
    case KEY_PGUP:
    case CTRL('u'): {
        size_t p = e->term_rows > 1 ? (e->term_rows - 1) / 2 : 1;
        e->cy = e->cy > p ? e->cy - p : 0;
        clamp_cx(e);
        break;
    }
    case KEY_PGDN:
    case CTRL('d'): {
        size_t p = e->term_rows > 1 ? (e->term_rows - 1) / 2 : 1;
        e->cy = e->cy + p < e->nlines ? e->cy + p : e->nlines - 1;
        clamp_cx(e);
        break;
    }
    case 'i':
        push_undo(e);
        e->vmode = M_INSERT;
        break;
    case 'a':
        push_undo(e);
        if (e->cx < e->line_len[e->cy]) {
            e->cx++;
        }
        e->vmode = M_INSERT;
        break;
    case 'I':
        push_undo(e);
        e->cx = 0;
        e->vmode = M_INSERT;
        break;
    case 'A':
        push_undo(e);
        e->cx = e->line_len[e->cy];
        e->vmode = M_INSERT;
        break;
    case 'o':
        push_undo(e);
        e->cx = e->line_len[e->cy];
        insert_newline(e);
        e->vmode = M_INSERT;
        break;
    case 'O':
        push_undo(e);
        e->cx = 0;
        insert_newline(e);
        if (e->cy > 0) {
            e->cy--;
        }
        e->vmode = M_INSERT;
        break;
    case 'x':
        push_undo(e);
        for (int i = 0; i < cnt; i++) {
            del_char(e);
        }
        clamp_cx(e);
        break;
    case 'D':
        push_undo(e);
        e->line_len[e->cy] = e->cx;
        e->lines[e->cy][e->cx] = '\0';
        e->dirty = true;
        break;
    case 'C':
        push_undo(e);
        e->line_len[e->cy] = e->cx;
        e->lines[e->cy][e->cx] = '\0';
        e->vmode = M_INSERT;
        e->dirty = true;
        break;
    case 'J':
        push_undo(e);
        vim_join(e);
        break;
    case 'p':
        push_undo(e);
        paste_lines(e, true);
        break;
    case 'P':
        push_undo(e);
        paste_lines(e, false);
        break;
    case 'u':
        do_undo(e);
        break;
    case 'd':
    case 'c':
    case 'g':
    case 'r':
    case 'y':
        e->vpending = k;
        e->vcount = cnt > 1 ? cnt : 0; // carry the count to the operator
        break;
    case 'n':
        do_search(e, 1);
        break;
    case 'N':
        do_search(e, -1);
        break;
    case ':':
        e->vmode = M_COMMAND;
        e->vcmd_len = 0;
        e->vcmd[0] = '\0';
        break;
    case '/':
        e->vmode = M_SEARCH;
        e->vcmd_len = 0;
        e->vcmd[0] = '\0';
        break;
    case CTRL('s'):
        set_status(e, save_file(e) ? "Saved" : "Save failed");
        break;
    case CTRL('x'):
        if (save_file(e)) {
            return EDITOR_RUN;
        }
        set_status(e, "Save failed");
        break;
    case CTRL('q'):
        if (e->dirty) {
            set_status(e, "Unsaved changes (:q! to discard, :wq to save)");
            break;
        }
        return EDITOR_QUIT;
    default:
        break;
    }
    return EDITOR_CONTINUE;
}

static void handle_insert(editor* e, int k)
{
    switch (k) {
    case KEY_ENTER:
        insert_newline(e);
        break;
    case KEY_BACKSPACE:
        do_backspace(e);
        break;
    case KEY_DEL:
        do_delete(e);
        break;
    case KEY_LEFT:
        if (e->cx > 0) {
            e->cx--;
        }
        break;
    case KEY_RIGHT:
        if (e->cx < e->line_len[e->cy]) {
            e->cx++;
        }
        break;
    case KEY_UP:
        if (e->cy > 0) {
            e->cy--;
            clamp_cx(e);
        }
        break;
    case KEY_DOWN:
        if (e->cy + 1 < e->nlines) {
            e->cy++;
            clamp_cx(e);
        }
        break;
    case KEY_HOME:
        e->cx = 0;
        break;
    case KEY_END:
        e->cx = e->line_len[e->cy];
        break;
    default:
        if (k >= 0x20 && k < 0x7F) {
            insert_char(e, (char)k);
        }
        break;
    }
}

// Esc / mode return.
static void vim_escape(editor* e)
{
    if (e->vmode == M_INSERT && e->cx > 0) {
        e->cx--; // vim steps left leaving insert
    }
    e->vmode = M_NORMAL;
    e->vpending = 0;
    e->vcount = 0;
}

static int handle_line_mode(editor* e, int k)
{
    if (k == 27) { // Esc cancels
        e->vmode = M_NORMAL;
        return EDITOR_CONTINUE;
    }
    if (k == KEY_ENTER || k == '\r' || k == '\n') {
        int m = e->vmode;
        e->vmode = M_NORMAL;
        if (m == M_SEARCH) {
            for (size_t i = 0; i <= e->vcmd_len; i++) {
                e->vsearch[i] = e->vcmd[i];
            }
            e->vsearch_len = e->vcmd_len;
            do_search(e, 1);
            return EDITOR_CONTINUE;
        }
        return run_command(e);
    }
    if (k == KEY_BACKSPACE) {
        if (e->vcmd_len > 0) {
            e->vcmd_len--;
            e->vcmd[e->vcmd_len] = '\0';
        } else {
            e->vmode = M_NORMAL;
        }
        return EDITOR_CONTINUE;
    }
    if (k >= 0x20 && k < 0x7F && e->vcmd_len < sizeof(e->vcmd) - 1) {
        e->vcmd[e->vcmd_len++] = (char)k;
        e->vcmd[e->vcmd_len] = '\0';
    }
    return EDITOR_CONTINUE;
}

// Dispatch one decoded key (a byte or a KEY_* code) by mode.
static int vim_key_logical(editor* e, int k)
{
    if (k == 27) {
        if (e->vmode == M_COMMAND || e->vmode == M_SEARCH) {
            e->vmode = M_NORMAL;
        } else {
            vim_escape(e);
        }
        return EDITOR_CONTINUE;
    }
    if (k != -1 && e->status_msg && e->vmode == M_NORMAL) {
        e->status_msg = NULL; // clear a transient message on the next action
    }
    switch (e->vmode) {
    case M_INSERT:
        handle_insert(e, k);
        return EDITOR_CONTINUE;
    case M_COMMAND:
    case M_SEARCH:
        return handle_line_mode(e, k);
    default:
        return handle_normal(e, k);
    }
}

// Byte-level VT100 decoder feeding vim_key_logical. Handles a lone Esc vs an
// ESC [ ... arrow sequence (resolved by a flush from the loop, byte < 0).
int editor_vim_key(editor* e, int byte)
{
    if (byte < 0) { // frame flush: resolve a dangling Esc as a lone Esc
        if (e->key_es == 1) {
            e->key_es = 0;
            return vim_key_logical(e, 27);
        }
        return EDITOR_CONTINUE;
    }
    if (e->key_es == 1) {
        if (byte == '[' || byte == 'O') {
            e->key_es = 2;
            e->key_csi_len = 0;
            return EDITOR_CONTINUE;
        }
        e->key_es = 0;
        int r = vim_key_logical(e, 27);
        if (r != EDITOR_CONTINUE) {
            return r;
        }
        return vim_key_logical(e, byte);
    }
    if (e->key_es == 2) {
        if (byte >= 0x40 && byte <= 0x7E) {
            e->key_es = 0;
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
                if (e->key_csi_len > 0 && e->key_csi[0] == '3') {
                    lk = KEY_DEL;
                } else if (e->key_csi_len > 0 && e->key_csi[0] == '5') {
                    lk = KEY_PGUP;
                } else if (e->key_csi_len > 0 && e->key_csi[0] == '6') {
                    lk = KEY_PGDN;
                } else if (e->key_csi_len > 0 &&
                           (e->key_csi[0] == '1' || e->key_csi[0] == '7')) {
                    lk = KEY_HOME;
                } else if (e->key_csi_len > 0 &&
                           (e->key_csi[0] == '4' || e->key_csi[0] == '8')) {
                    lk = KEY_END;
                }
                break;
            default:
                break;
            }
            return lk >= 0 ? vim_key_logical(e, lk) : EDITOR_CONTINUE;
        }
        if (e->key_csi_len < (int)sizeof(e->key_csi) - 1) {
            e->key_csi[e->key_csi_len++] = (char)byte;
        }
        return EDITOR_CONTINUE;
    }
    if (byte == 27) {
        e->key_es = 1;
        return EDITOR_CONTINUE;
    }
    // Normalize the control bytes the handlers expect as logical keys.
    if (byte == '\r' || byte == '\n') {
        return vim_key_logical(e, KEY_ENTER);
    }
    if (byte == 0x7F || byte == 0x08) {
        return vim_key_logical(e, KEY_BACKSPACE);
    }
    return vim_key_logical(e, byte);
}

// --- lifecycle
// ----------------------------------------------------------------

editor* editor_open(allocator* mem, const char* path)
{
    editor* e = new (mem, editor, 1);
    e->mem = mem;
    e->lines = new (mem, ed_line, ED_MAX_LINES);
    e->line_len = new (mem, size_t, ED_MAX_LINES);
    e->hl = new (mem, char, ED_HL_MAX + 1);
    // scr / undo slots / yank are allocated from the arena on first use.
    load_file(e, path);
    return e; // everything else is zeroed by the allocator contract
}

// --- windowed rendering
// -------------------------------------------------------

static mu_Color vrgb(uint32_t c)
{
    return mu_color((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, 255);
}

void editor_vim_draw(editor* e, mu_Context* ctx)
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
    e->term_cols = (size_t)cols;
    e->term_rows = (size_t)rows;
    scroll_to_cursor(e);

    int textrows = rows - 1;
    size_t width = view_width(e);
    for (int y = 0; y < textrows; y++) {
        size_t row = e->rowoff + (size_t)y;
        if (row >= e->nlines) {
            break;
        }
        size_t len = e->line_len[row];
        if (e->coloff < len) {
            size_t vis = len - e->coloff;
            if (vis > width) {
                vis = width;
            }
            highlight_lua(e->lines[row] + e->coloff, vis, e->hl, ED_HL_MAX);
            ui_text_ansi(ctx, e->hl, b.x, b.y + y * VGH);
        }
    }

    // Cursor: block in NORMAL, thin bar in INSERT.
    int cxs = (int)(e->cx - e->coloff);
    int cys = (int)(e->cy - e->rowoff);
    int px = b.x + cxs * VGW;
    int py = b.y + cys * VGH;
    if (e->vmode == M_INSERT) {
        mu_draw_rect(ctx, mu_rect(px, py, 2, VGH), vrgb(0x9ecbff));
    } else if (e->vmode == M_NORMAL) {
        mu_draw_rect(ctx, mu_rect(px, py, VGW, VGH), vrgb(0x9ecbff));
        if (e->cy < e->nlines && e->cx < e->line_len[e->cy]) {
            char ch[1] = {e->lines[e->cy][e->cx]};
            mu_draw_text(ctx, NULL, ch, 1, mu_vec2(px, py), vrgb(0x0e1116));
        }
    }

    // Status bar.
    int sy = b.y + textrows * VGH;
    mu_draw_rect(ctx, mu_rect(b.x, sy, b.w, VGH), vrgb(0x24406a));
    char st[256];
    if (e->vmode == M_COMMAND) {
        snprintf(st, sizeof st, ":%.*s", (int)e->vcmd_len, e->vcmd);
    } else if (e->vmode == M_SEARCH) {
        snprintf(st, sizeof st, "/%.*s", (int)e->vcmd_len, e->vcmd);
    } else {
        const char* mn = e->vmode == M_INSERT ? "-- INSERT --" : "-- NORMAL --";
        snprintf(st, sizeof st, "%s  %s%s   Ln %u, Col %u%s%s", mn, e->filepath,
                 e->dirty ? " *" : "", (unsigned)(e->cy + 1),
                 (unsigned)(e->cx + 1), e->status_msg ? "   " : "",
                 e->status_msg ? e->status_msg : "");
    }
    size_t stn = 0;
    while (st[stn] && (int)stn < cols) {
        stn++;
    }
    mu_draw_text(ctx, NULL, st, (int)stn, mu_vec2(b.x, sy), vrgb(0xffffff));
}
