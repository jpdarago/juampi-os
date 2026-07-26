// Graphical UI layer (see ui.h): a renderer + input pump that drive the
// vendored microui over the shell framebuffer. Popups run as a modal loop
// mirroring the full-screen editor (src/editor.c): each frame we pump the PS/2
// mouse/keyboard into microui, let a build callback emit the UI, render
// microui's command list through the clip-aware gfx primitives, draw the
// cursor, and flip. The shell text underneath is snapshotted once and repainted
// every frame, so windows float over the live REPL and it is restored intact on
// close.

#include <ui.h>
#include <gfx.h>
#include <mouse.h>
#include <keyboard.h>
#include <serial.h>
#include <memory.h>
#include <term.h>
#include <console.h>
#include <luashell.h>
#include <fault.h>
#include <net.h>
#include <editor.h>
#include <arena.h>

#include <printf/printf.h> // snprintf for the editor window title
#include <stdint.h>
#include <stddef.h>

#include "microui/microui.h"

#define GLYPH_W 8
#define GLYPH_H 16

static size_t ui_strlen(const char* s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

// --- root allocator (injected) + per-widget arenas --------------------------

// The UI's root allocator, injected once at boot (shell_run -> ui_init). The UI
// never calls heap_default(); per-widget arenas and the microui context are
// carved from this.
static heap_allocator* ui_heap;

void ui_init(heap_allocator* h)
{
    ui_heap = h;
}
heap_allocator* ui_root_heap(void)
{
    return ui_heap;
}

// A lifetime-scoped arena for a widget: one block from the injected root heap,
// wrapped in an arena the widget allocates from, freed wholesale on close.
typedef struct {
    arena a;
    void* backing;
} ui_arena;

static ui_arena ui_arena_new(ptrdiff_t size)
{
    // 16 is the heap's maximum alignment; the arena pads its own allocations.
    void* backing = alloc(&ui_heap->base, size, 16, 1);
    ui_arena ua = {arena_init(backing, size), backing};
    return ua;
}
static void ui_arena_free(ui_arena* ua)
{
    heap_free(ui_heap, ua->backing);
    ua->backing = NULL;
}

// --- microui context --------------------------------------------------------

static mu_Context* g_ctx;
static bool in_frame;

static int text_width_cb(mu_Font font, const char* str, int len)
{
    (void)font;
    if (len < 0) {
        len = (int)ui_strlen(str);
    }
    return len * GLYPH_W;
}
static int text_height_cb(mu_Font font)
{
    (void)font;
    return GLYPH_H;
}

static mu_Color col(int r, int g, int b)
{
    mu_Color c = {(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
    return c;
}

// A bright, blue-title-bar theme reminiscent of classic desktop / TempleOS UIs,
// sized so the 16px font fits the controls.
static void apply_theme(mu_Context* ctx)
{
    mu_Style* s = ctx->style;
    s->size.y = 22;
    s->padding = 6;
    s->spacing = 4;
    s->title_height = 28;
    s->scrollbar_size = 14;
    s->thumb_size = 10;
    s->colors[MU_COLOR_TEXT] = col(20, 20, 20);
    s->colors[MU_COLOR_BORDER] = col(30, 30, 40);
    s->colors[MU_COLOR_WINDOWBG] = col(224, 224, 224);
    s->colors[MU_COLOR_TITLEBG] = col(0, 0, 160);
    s->colors[MU_COLOR_TITLETEXT] = col(255, 255, 255);
    s->colors[MU_COLOR_PANELBG] = col(208, 208, 208);
    s->colors[MU_COLOR_BUTTON] = col(180, 180, 190);
    s->colors[MU_COLOR_BUTTONHOVER] = col(150, 170, 210);
    s->colors[MU_COLOR_BUTTONFOCUS] = col(120, 150, 220);
    s->colors[MU_COLOR_BASE] = col(255, 255, 255);
    s->colors[MU_COLOR_BASEHOVER] = col(235, 235, 245);
    s->colors[MU_COLOR_BASEFOCUS] = col(220, 225, 245);
    s->colors[MU_COLOR_SCROLLBASE] = col(200, 200, 200);
    s->colors[MU_COLOR_SCROLLTHUMB] = col(120, 120, 140);
}

static mu_Context* ui_ctx(void)
{
    if (g_ctx == NULL) {
        g_ctx = new (&ui_heap->base, mu_Context, 1);
        mu_init(g_ctx);
        g_ctx->text_width = text_width_cb;
        g_ctx->text_height = text_height_cb;
        apply_theme(g_ctx);
    }
    return g_ctx;
}

bool ui_available(void)
{
    return gfx_available();
}

mu_Context* ui_current(void)
{
    return in_frame ? g_ctx : NULL;
}

// --- rendering --------------------------------------------------------------

// A custom microui command: blit a native-layout pixel buffer (a Lua canvas)
// into a window. Pushed by ui_image(), handled in render().
#define CMD_IMAGE (MU_COMMAND_MAX + 1)
typedef struct {
    mu_BaseCommand base;
    const uint32_t* buf;
    int w, h;
    mu_Rect rect;
} ImageCommand;

void ui_image(mu_Context* ctx, const uint32_t* buf, int w, int h)
{
    mu_Container* cnt = mu_get_current_container(ctx);
    if (cnt == NULL) {
        return;
    }
    mu_Rect b = cnt->body;
    // Dark letterbox behind the image so any body the canvas doesn't cover
    // reads as intentional, not the light window background.
    mu_draw_rect(ctx, b, mu_color(14, 17, 22, 255));
    ImageCommand* c = (ImageCommand*)mu_push_command(ctx, CMD_IMAGE,
                                                     sizeof(ImageCommand));
    c->buf = buf;
    c->w = w;
    c->h = h;
    c->rect = mu_rect(b.x, b.y, w, h);
}

static uint32_t rgb(mu_Color c)
{
    return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}

static mu_Color colu(uint32_t c)
{
    return mu_color((int)((c >> 16) & 0xff), (int)((c >> 8) & 0xff),
                    (int)(c & 0xff), 255);
}

// Draw an ANSI-SGR-colored string (the 5 highlighter codes) as colored text
// runs starting at pixel (x, y). Used by the windowed editor's highlighted
// lines.
void ui_text_ansi(mu_Context* ctx, const char* s, int x, int y)
{
    static const uint32_t pal[5] = {0xd4d4d4, 0x6ac46a, 0xd4c46a, 0xc46ac4,
                                    0x808080};
    int color = 0, esc = 0, csi_len = 0, n = 0, col = 0;
    char csi[16], run[256];
    for (int i = 0; s[i] != '\0'; i++) {
        char ch = s[i];
        if (esc == 1) {
            esc = (ch == '[') ? 2 : 0;
            csi_len = 0;
        } else if (esc == 2) {
            if (ch >= 0x40 && ch <= 0x7e) {
                if (ch == 'm') {
                    if (n > 0) {
                        mu_draw_text(ctx, NULL, run, n,
                                     mu_vec2(x + (col - n) * GLYPH_W, y),
                                     colu(pal[color]));
                        n = 0;
                    }
                    int code = 0;
                    for (int j = 0; j < csi_len; j++) {
                        code = code * 10 + (csi[j] - '0');
                    }
                    color = (code == 32)   ? 1
                            : (code == 33) ? 2
                            : (code == 35) ? 3
                            : (code == 90) ? 4
                                           : 0;
                }
                esc = 0;
            } else if (csi_len < (int)sizeof(csi) - 1) {
                csi[csi_len++] = ch;
            }
        } else if (ch == 27) {
            esc = 1;
        } else if (n < (int)sizeof(run)) {
            run[n++] = ch;
            col++;
        }
    }
    if (n > 0) {
        mu_draw_text(ctx, NULL, run, n, mu_vec2(x + (col - n) * GLYPH_W, y),
                     colu(pal[color]));
    }
}

// microui icons drawn as centered glyphs (no atlas): a close cross, a checkbox
// tick, and treenode collapse/expand markers.
static void draw_icon(gfx_surface* s, int id, mu_Rect r, mu_Color c)
{
    char ch = '?';
    switch (id) {
    case MU_ICON_CLOSE:
        ch = 'x';
        break;
    case MU_ICON_CHECK:
        ch = 'x';
        break;
    case MU_ICON_COLLAPSED:
        ch = '+';
        break;
    case MU_ICON_EXPANDED:
        ch = '-';
        break;
    default:
        return;
    }
    int gx = r.x + (r.w - GLYPH_W) / 2;
    int gy = r.y + (r.h - GLYPH_H) / 2;
    gfx_glyph(s, gx, gy, (unsigned char)ch, rgb(c));
}

static void render(mu_Context* ctx)
{
    gfx_surface* s = gfx_screen();
    mu_Command* cmd = NULL;
    while (mu_next_command(ctx, &cmd)) {
        switch (cmd->type) {
        case MU_COMMAND_RECT: {
            mu_Rect r = cmd->rect.rect;
            gfx_fill(s, r.x, r.y, r.w, r.h, rgb(cmd->rect.color));
            break;
        }
        case MU_COMMAND_TEXT: {
            mu_TextCommand* t = &cmd->text;
            gfx_text(s, t->pos.x, t->pos.y, t->str, ui_strlen(t->str),
                     rgb(t->color));
            break;
        }
        case MU_COMMAND_ICON:
            draw_icon(s, cmd->icon.id, cmd->icon.rect, cmd->icon.color);
            break;
        case MU_COMMAND_CLIP: {
            mu_Rect r = cmd->clip.rect;
            gfx_clip(s, r.x, r.y, r.w, r.h);
            break;
        }
        case CMD_IMAGE: {
            ImageCommand* ic = (ImageCommand*)cmd;
            gfx_image(s, ic->rect.x, ic->rect.y, ic->w, ic->h, ic->buf);
            break;
        }
        default:
            break;
        }
    }
}

// A classic left-pointing arrow cursor: '#' outline, '.' fill, ' ' transparent.
// Drawn last, straight to the screen (gfx_pixel clips to the display).
static const char* const CURSOR[] = {
        "#",         "##",       "#.#",      "#..#",      "#...#",
        "#....#",    "#.....#",  "#......#", "#.......#", "#........#",
        "#....####", "#..#..#",  "#.# #..#", "##  #..#",  "#    #..#",
        "     #..#", "      ##",
};

static void draw_cursor(int x, int y)
{
    int rows = (int)(sizeof CURSOR / sizeof CURSOR[0]);
    for (int j = 0; j < rows; j++) {
        const char* row = CURSOR[j];
        for (int i = 0; row[i] != '\0'; i++) {
            if (row[i] == '.') {
                gfx_pixel(x + i, y + j, 0xffffff);
            } else if (row[i] == '#') {
                gfx_pixel(x + i, y + j, 0x000000);
            }
        }
    }
}

// --- input pump -------------------------------------------------------------

static int cur_x, cur_y; // cursor position, in screen pixels
static uint8_t prev_btn; // previous mouse button bitmask (for edge detection)
static int esc_state;    // 0 normal, 1 saw ESC, 2 in a CSI escape sequence

static void feed_byte(mu_Context* ctx, int c, bool* want_close)
{
    if (esc_state == 1) {
        if (c == '[' || c == 'O') {
            esc_state = 2;
            return;
        }
        // Lone ESC immediately followed by another key: treat ESC as "close",
        // then fall through and handle this byte normally.
        *want_close = true;
        esc_state = 0;
    }
    if (esc_state == 2) {
        if (c >= 0x40 && c <= 0x7E) { // final byte of the sequence
            if (c == 'A') {
                mu_input_scroll(ctx, 0, -GLYPH_H * 3);
            } else if (c == 'B') {
                mu_input_scroll(ctx, 0, GLYPH_H * 3);
            }
            esc_state = 0;
        }
        return;
    }
    if (c == 27) {
        esc_state = 1;
    } else if (c == '\r' || c == '\n') {
        mu_input_keydown(ctx, MU_KEY_RETURN);
        mu_input_keyup(ctx, MU_KEY_RETURN);
    } else if (c == '\b' || c == 127) {
        mu_input_keydown(ctx, MU_KEY_BACKSPACE);
        mu_input_keyup(ctx, MU_KEY_BACKSPACE);
    } else if (c >= 32 && c < 127) {
        char t[2] = {(char)c, 0};
        mu_input_text(ctx, t);
    }
}

// Integrate accumulated mouse movement into the cursor and feed microui the
// motion + button edges. Shared by the modal and desktop loops.
// Mild pointer acceleration: precise for small moves, faster for quick flicks,
// so a relative PS/2 mouse can still reach the screen corners without huge
// swipes.
static int accel(int d)
{
    int a = d < 0 ? -d : d;
    return d + d * a / 6;
}

static void feed_mouse(mu_Context* ctx)
{
    int dx = 0, dy = 0;
    uint8_t btn = 0;
    mouse_poll(&dx, &dy, &btn);
    cur_x += accel(dx);
    cur_y += accel(dy);
    if (cur_x < 0) {
        cur_x = 0;
    }
    if (cur_y < 0) {
        cur_y = 0;
    }
    if (cur_x >= (int)gfx_width()) {
        cur_x = (int)gfx_width() - 1;
    }
    if (cur_y >= (int)gfx_height()) {
        cur_y = (int)gfx_height() - 1;
    }
    mu_input_mousemove(ctx, cur_x, cur_y);

    static const uint8_t bits[3] = {0x01, 0x02, 0x04};
    static const int mubtn[3] = {MU_MOUSE_LEFT, MU_MOUSE_RIGHT,
                                 MU_MOUSE_MIDDLE};
    for (int i = 0; i < 3; i++) {
        if ((btn & bits[i]) && !(prev_btn & bits[i])) {
            mu_input_mousedown(ctx, cur_x, cur_y, mubtn[i]);
        } else if (!(btn & bits[i]) && (prev_btn & bits[i])) {
            mu_input_mouseup(ctx, cur_x, cur_y, mubtn[i]);
        }
    }
    prev_btn = btn;
}

// Modal pump: mouse to microui, keyboard to microui's text/key input. Returns
// true if a bare Esc asked to close the modal.
static bool pump_input(mu_Context* ctx)
{
    bool want_close = false;
    feed_mouse(ctx);
    int c;
    while ((c = keyboard_poll()) >= 0) {
        feed_byte(ctx, c, &want_close);
    }
    while ((c = serial_poll()) >= 0) {
        feed_byte(ctx, c, &want_close);
    }
    // A bare ESC with nothing following it this pass means "close".
    if (esc_state == 1) {
        want_close = true;
        esc_state = 0;
    }
    return want_close;
}

// --- modal loop -------------------------------------------------------------

void ui_run(ui_frame_fn build, void* ud)
{
    if (!gfx_available()) {
        return;
    }
    mu_Context* ctx = ui_ctx();

    bool was_buffered = gfx_buffered();
    if (!was_buffered) {
        gfx_buffer(true);
    }
    gfx_snapshot(); // capture the shell image to repaint under the windows

    cur_x = (int)gfx_width() / 2;
    cur_y = (int)gfx_height() / 2;
    prev_btn = 0;
    esc_state = 0;

    bool running = true;
    while (running) {
        bool close = pump_input(ctx);

        mu_begin(ctx);
        in_frame = true;
        bool keep = build(ctx, ud);
        in_frame = false;
        mu_end(ctx);

        if (close || !keep) {
            running = false;
        }

        gfx_restore(); // shell background
        gfx_clip_reset(gfx_screen());
        render(ctx);
        gfx_clip_reset(gfx_screen());
        draw_cursor(cur_x, cur_y);
        gfx_flip();

        if (running) {
            __asm__ __volatile__("hlt");
        }
    }

    // Drop the UI: repaint the clean shell image and stop buffering.
    gfx_restore();
    gfx_flip();
    if (!was_buffered) {
        gfx_buffer(false);
    }
    gfx_snapshot_free();
}

// --- native convenience popups ----------------------------------------------

struct msg_ud {
    const char* title;
    const char* body;
};

static bool msg_frame(mu_Context* ctx, void* ud)
{
    struct msg_ud* m = ud;
    int w = (int)gfx_width();
    int h = (int)gfx_height();
    int ww = w * 3 / 4;
    int wh = h * 3 / 4;
    int open = mu_begin_window(ctx, m->title,
                               mu_rect((w - ww) / 2, (h - wh) / 2, ww, wh));
    if (open) {
        int fill[1] = {-1};
        mu_layout_row(ctx, 1, fill, -1);
        mu_text(ctx, m->body);
        mu_end_window(ctx);
    }
    return open != 0;
}

void ui_message(const char* title, const char* body)
{
    struct msg_ud m = {title, body};
    ui_run(msg_frame, &m);
}

struct confirm_ud {
    const char* title;
    const char* body;
    bool result;
    bool done;
};

static bool confirm_frame(mu_Context* ctx, void* ud)
{
    struct confirm_ud* c = ud;
    int w = (int)gfx_width();
    int h = (int)gfx_height();
    int ww = 420;
    int wh = 200;
    int open = mu_begin_window_ex(ctx, c->title,
                                  mu_rect((w - ww) / 2, (h - wh) / 2, ww, wh),
                                  MU_OPT_NORESIZE);
    if (open) {
        int fill[1] = {-1};
        mu_layout_row(ctx, 1, fill, -1);
        mu_text(ctx, c->body);
        int btns[2] = {-90, -1};
        mu_layout_row(ctx, 2, btns, 0);
        if (mu_button(ctx, "OK")) {
            c->result = true;
            c->done = true;
        }
        if (mu_button(ctx, "Cancel")) {
            c->result = false;
            c->done = true;
        }
        mu_end_window(ctx);
    }
    return open != 0 && !c->done;
}

bool ui_confirm(const char* title, const char* body)
{
    struct confirm_ud c = {title, body, false, false};
    ui_run(confirm_frame, &c);
    return c.result;
}

// --- desktop (persistent windowed shell) ------------------------------------

#define DESKTOP_BG 0x2e4a5e

// The Lua layer (lua_ui.c) registers a hook that builds the non-modal windows
// (help, tool windows) each frame; kept behind a pointer so ui.c stays
// Lua-agnostic.
static void (*window_hook)(mu_Context*);

void ui_set_window_hook(void (*fn)(mu_Context*))
{
    window_hook = fn;
}

// Canvas windows opened from C (a native lab program that drew into an
// off-screen buffer — see run() in lua_run.c). ui.c owns the buffer and frees
// it when the window is closed. Distinct from the Lua ui.open() registry.
#define MAXCANV 4
static struct {
    bool used;
    bool fresh; // just (re)opened — force the retained container open
    char title[48];
    uint32_t* buf;
    int w, h;
} canv[MAXCANV];

void ui_open_canvas(const char* title, uint32_t* buf, int w, int h)
{
    int slot = -1;
    for (int i = 0; i < MAXCANV; i++) {
        if (canv[i].used) {
            bool same = true;
            for (int k = 0; k < 47 && (title[k] || canv[i].title[k]); k++) {
                if (title[k] != canv[i].title[k]) {
                    same = false;
                    break;
                }
            }
            if (same) {
                slot = i; // re-render into the same window: free the old buffer
                heap_free(ui_heap, canv[i].buf);
                break;
            }
        } else if (slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        heap_free(ui_heap, buf); // no room
        return;
    }
    canv[slot].used = true;
    canv[slot].fresh = true;
    canv[slot].buf = buf;
    canv[slot].w = w;
    canv[slot].h = h;
    int i = 0;
    for (; i < 47 && title[i]; i++) {
        canv[slot].title[i] = title[i];
    }
    canv[slot].title[i] = '\0';
}

static void draw_canvas_windows(mu_Context* ctx)
{
    for (int i = 0; i < MAXCANV; i++) {
        if (!canv[i].used) {
            continue;
        }
        int x = 90 + 24 * i;
        int y = 70 + 24 * i;
        mu_Rect r = mu_rect(x, y, canv[i].w, canv[i].h + 28);
        // Force the retained container open the frame after (re)opening.
        if (canv[i].fresh) {
            mu_Container* c = mu_get_container(ctx, canv[i].title);
            if (c != NULL) {
                c->open = 1;
            }
            canv[i].fresh = false;
        }
        if (!mu_begin_window(ctx, canv[i].title, r)) {
            heap_free(ui_heap, canv[i].buf); // closed via [x]
            canv[i].buf = NULL;
            canv[i].used = false;
            continue;
        }
        ui_image(ctx, canv[i].buf, canv[i].w, canv[i].h);
        mu_end_window(ctx);
    }
}

static void desktop_windows(mu_Context* ctx)
{
    if (window_hook != NULL) {
        window_hook(ctx);
    }
    draw_canvas_windows(ctx);
}

// Suspend the desktop compositor so a full-screen activity (the text editor, a
// framebuffer demo) can own the raw screen: detach the terminal sink (output
// goes back to flanterm), stop double-buffering, and wipe the screen. Resume
// restores buffering + the sink; the desktop loop repaints on the next frame.
// The desktop's single terminal instance (its state lives in a per-terminal
// arena; this pointer lets ui_fullscreen_end re-attach the console sink to it).
static term* desk_term;

// The terminal's whole lifetime: the ~430 KB scrollback grid + input/history,
// with headroom. Lives for the desktop's life (never freed).
#define TERM_ARENA_SIZE ((ptrdiff_t)1024 * 1024)

void ui_fullscreen_begin(void)
{
    if (!gfx_available()) {
        return;
    }
    console_set_sink(NULL, NULL);
    gfx_buffer(false);
    console_clear();
}

void ui_fullscreen_end(void)
{
    if (!gfx_available()) {
        return;
    }
    gfx_buffer(true);
    console_set_sink(term_write, desk_term);
}

void ui_desktop_run(void)
{
    if (!gfx_available()) {
        return; // headless: caller falls back to the classic text REPL
    }
    mu_Context* ctx = ui_ctx();
    ui_arena ta = ui_arena_new(TERM_ARENA_SIZE); // desktop-lifetime, not freed
    desk_term = term_open(&ta.a.base);
    console_set_sink(term_write,
                     desk_term); // shell output now lands in the terminal grid
    luashell_init();             // prelude/init.lua greetings land in the grid
    console_print("juampiOS desktop - a Lua 5.4 shell in a window.\n"
                  "  help() opens the reference;  drag windows by the title "
                  "bar.\n\n");

    gfx_buffer(true);
    cur_x = (int)gfx_width() / 2;
    cur_y = (int)gfx_height() / 2;
    prev_btn = 0;

    // A CPU fault while evaluating a line longjmps here: reset the interpreter
    // and keep the desktop running (mirrors the classic shell's recovery).
    if (setjmp(fault_env) != 0) {
        __asm__ __volatile__("sti");
        console_print("\n[recovered from fault: vector ");
        console_dec(fault_vector);
        console_print(", rip ");
        console_hex(fault_rip);
        console_print(" - interpreter reset]\n");
        luashell_init();
    }

    for (;;) {
        feed_mouse(ctx);
        int c;
        while ((c = keyboard_poll()) >= 0) {
            term_key(desk_term, c);
        }
        while ((c = serial_poll()) >= 0) {
            term_key(desk_term, c);
        }

        mu_begin(ctx);
        in_frame = true;
        term_build(desk_term, ctx);
        desktop_windows(ctx);
        in_frame = false;
        mu_end(ctx);

        gfx_clip_reset(gfx_screen());
        gfx_fill(gfx_screen(), 0, 0, (int)gfx_width(), (int)gfx_height(),
                 DESKTOP_BG);
        render(ctx);
        gfx_clip_reset(gfx_screen());
        draw_cursor(cur_x, cur_y);
        gfx_flip();

        net_poll(); // keep the network stack live between keystrokes
        __asm__ __volatile__("hlt");
    }
}

// --- windowed editor (modal) ------------------------------------------------

// Everything an editor allocates over its lifetime — document lines, undo
// slots, yank register, frame scratch — with headroom. Freed wholesale below.
#define EDITOR_ARENA_SIZE ((ptrdiff_t)2 * 1024 * 1024)

int ui_edit(const char* path)
{
    // The editor's whole lifetime lives in one arena, whether windowed or
    // headless; ui_arena_free at the end releases every buffer at once.
    ui_arena ua = ui_arena_new(EDITOR_ARENA_SIZE);
    editor* e = editor_open(&ua.a.base, path);

    if (!gfx_available()) {
        int r = editor_run(e); // classic full-screen editor (headless)
        ui_arena_free(&ua);
        return r;
    }
    mu_Context* ctx = ui_ctx();

    char title[160];
    snprintf(title, sizeof title, "edit: %s", path);

    bool was_buffered = gfx_buffered();
    if (!was_buffered) {
        gfx_buffer(true);
    }
    gfx_snapshot(); // float the editor window over the frozen desktop
    cur_x = (int)gfx_width() / 2;
    cur_y = (int)gfx_height() / 2;
    prev_btn = 0;

    int W = (int)gfx_width();
    int H = (int)gfx_height();
    int ww = W * 3 / 4;
    int wh = H * 3 / 4;
    int action = EDITOR_CONTINUE;
    bool fresh = true;

    while (action == EDITOR_CONTINUE) {
        feed_mouse(ctx); // window drag/resize
        int c;
        while (action == EDITOR_CONTINUE && (c = keyboard_poll()) >= 0) {
            action = editor_vim_key(e, c);
        }
        while (action == EDITOR_CONTINUE && (c = serial_poll()) >= 0) {
            action = editor_vim_key(e, c);
        }
        if (action == EDITOR_CONTINUE) {
            action = editor_vim_key(e, -1); // resolve a dangling Esc
        }

        mu_begin(ctx);
        in_frame = true;
        if (fresh) {
            mu_Container* cc = mu_get_container(ctx, title);
            if (cc != NULL) {
                cc->open = 1;
            }
            fresh = false;
        }
        if (mu_begin_window(ctx, title,
                            mu_rect((W - ww) / 2, (H - wh) / 2, ww, wh))) {
            editor_vim_draw(e, ctx);
            mu_end_window(ctx);
        } else if (action == EDITOR_CONTINUE) {
            action = EDITOR_QUIT; // window closed via the titlebar [x]
        }
        in_frame = false;
        mu_end(ctx);

        gfx_restore();
        gfx_clip_reset(gfx_screen());
        render(ctx);
        gfx_clip_reset(gfx_screen());
        draw_cursor(cur_x, cur_y);
        gfx_flip();

        if (action == EDITOR_CONTINUE) {
            __asm__ __volatile__("hlt");
        }
    }

    gfx_restore();
    gfx_flip();
    if (!was_buffered) {
        gfx_buffer(false);
    }
    gfx_snapshot_free();
    ui_arena_free(&ua); // releases the editor and all its buffers at once
    return action;
}
