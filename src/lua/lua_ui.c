// The `ui` library: an immediate-mode GUI for Lua, backed by microui + src/ui.c.
//
//   ui.window("Title", function()
//     ui.label("Hello")
//     if ui.button("OK") then print("clicked") end
//   end)
//
// ui.window runs a modal loop (src/ui.c) and calls the Lua function once per
// frame; the widget wrappers below emit microui controls into the current frame.
// ui.popup/alert/confirm are one-shot native dialogs. All are no-ops (or return
// a graceful fallback) when there is no framebuffer.

#include <ui.h>
#include <gfx.h>
#include <console.h>
#include <memory.h>

#include "lua.h"
#include "lauxlib.h"

#include "../microui/microui.h"

// Expose a raw buffer as a Lua `mem` view (defined in lua_thread.c, also used by
// fb.canvas). Lets a canvas feed thread.parallel for the raytracer.
void mem_push_view(lua_State* L, void* ptr, size_t size);

// The active microui context, or a Lua error if a widget is used outside a frame
// (i.e. outside the function passed to ui.window).
static mu_Context* need_ctx(lua_State* L)
{
    mu_Context* c = ui_current();
    if (c == NULL) {
        luaL_error(L, "ui: widget called outside ui.window()");
    }
    return c;
}

// --- the modal window: ui.window(title, fn) ---------------------------------

struct win_ud {
    lua_State* L;
    int ref;           // registry ref to the Lua build function
    const char* title;
    bool err;          // the build function raised an error
};

static bool win_frame(mu_Context* ctx, void* ud)
{
    struct win_ud* w = ud;
    int wi = (int)gfx_width();
    int hi = (int)gfx_height();
    int ww = wi * 2 / 3;
    int wh = hi * 2 / 3;
    int open = mu_begin_window(ctx, w->title,
                               mu_rect((wi - ww) / 2, (hi - wh) / 2, ww, wh));
    if (open) {
        lua_rawgeti(w->L, LUA_REGISTRYINDEX, w->ref);
        if (lua_pcall(w->L, 0, 0, 0) != LUA_OK) {
            w->err = true; // error object left on the stack for l_window
            mu_end_window(ctx);
            return false;
        }
        mu_end_window(ctx);
    }
    return open != 0;
}

static int l_window(lua_State* L)
{
    const char* title = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!ui_available()) {
        lua_pushnil(L);
        lua_pushstring(L, "ui: no framebuffer");
        return 2;
    }
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    struct win_ud w = {L, ref, title, false};
    ui_run(win_frame, &w);
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    if (w.err) {
        return lua_error(L); // re-raise the message the build function left
    }
    return 0;
}

// --- non-modal desktop windows: ui.open(title, fn) / ui.close(id) -----------
// Registered windows persist on the desktop and coexist with the shell; the
// desktop loop calls build_windows() each frame (via ui_set_window_hook).

#define MAXW 8
static struct {
    bool used;
    bool fresh; // just (re)opened — force the retained container open this frame
    lua_State* L;
    int ref;   // registry ref to the build function
    int id;
    int w, h;  // requested window size (0 = default)
    char title[64];
} deskw[MAXW];
static int desk_gid = 1;

static void copy_title(char* dst, const char* s)
{
    int i = 0;
    for (; s[i] && i < 63; i++) {
        dst[i] = s[i];
    }
    dst[i] = '\0';
}

static bool title_eq(const char* a, const char* b)
{
    int i = 0;
    while (a[i] && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

// ui.open(title, fn [, w, h]) — w,h size the window (default when omitted).
static int l_open(lua_State* L)
{
    const char* title = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    int w = (int)luaL_optinteger(L, 3, 0);
    int h = (int)luaL_optinteger(L, 4, 0);
    if (!ui_available()) {
        lua_pushnil(L);
        lua_pushstring(L, "ui: no framebuffer");
        return 2;
    }
    // De-dupe by title: re-opening refreshes the existing window's builder.
    for (int i = 0; i < MAXW; i++) {
        if (deskw[i].used && title_eq(deskw[i].title, title)) {
            luaL_unref(L, LUA_REGISTRYINDEX, deskw[i].ref);
            lua_pushvalue(L, 2);
            deskw[i].ref = luaL_ref(L, LUA_REGISTRYINDEX);
            deskw[i].L = L;
            deskw[i].w = w;
            deskw[i].h = h;
            deskw[i].fresh = true;
            lua_pushinteger(L, deskw[i].id);
            return 1;
        }
    }
    for (int i = 0; i < MAXW; i++) {
        if (!deskw[i].used) {
            deskw[i].used = true;
            deskw[i].fresh = true;
            deskw[i].L = L;
            deskw[i].w = w;
            deskw[i].h = h;
            copy_title(deskw[i].title, title);
            lua_pushvalue(L, 2);
            deskw[i].ref = luaL_ref(L, LUA_REGISTRYINDEX);
            deskw[i].id = desk_gid++;
            lua_pushinteger(L, deskw[i].id);
            return 1;
        }
    }
    lua_pushnil(L);
    lua_pushstring(L, "ui: too many open windows");
    return 2;
}

static int l_close(lua_State* L)
{
    int id = (int)luaL_checkinteger(L, 1);
    for (int i = 0; i < MAXW; i++) {
        if (deskw[i].used && deskw[i].id == id) {
            luaL_unref(deskw[i].L, LUA_REGISTRYINDEX, deskw[i].ref);
            deskw[i].used = false;
            break;
        }
    }
    return 0;
}

// Invoked by the desktop loop each frame to render the registered windows.
static void build_windows(mu_Context* ctx)
{
    int W = (int)gfx_width();
    int H = (int)gfx_height();
    for (int i = 0; i < MAXW; i++) {
        if (!deskw[i].used) {
            continue;
        }
        int ww = deskw[i].w > 0 ? deskw[i].w : W / 2;
        int wh = deskw[i].h > 0 ? deskw[i].h : H * 3 / 5;
        int x = W - ww - 40 - 30 * i;
        int y = 60 + 30 * i;
        if (x < 40) {
            x = 40;
        }
        // A reopened window reuses microui's retained container, which remembers
        // it was closed — force it open on the first frame after (re)opening.
        if (deskw[i].fresh) {
            mu_Container* c = mu_get_container(ctx, deskw[i].title);
            if (c != NULL) {
                c->open = 1;
            }
            deskw[i].fresh = false;
        }
        if (!mu_begin_window(ctx, deskw[i].title, mu_rect(x, y, ww, wh))) {
            // Closed via the titlebar [x]: drop it.
            luaL_unref(deskw[i].L, LUA_REGISTRYINDEX, deskw[i].ref);
            deskw[i].used = false;
            continue;
        }
        lua_rawgeti(deskw[i].L, LUA_REGISTRYINDEX, deskw[i].ref);
        if (lua_pcall(deskw[i].L, 0, 0, 0) != LUA_OK) {
            console_print("ui window error: ");
            console_print(lua_tostring(deskw[i].L, -1));
            console_print("\n");
            lua_pop(deskw[i].L, 1);
            luaL_unref(deskw[i].L, LUA_REGISTRYINDEX, deskw[i].ref);
            deskw[i].used = false;
        }
        mu_end_window(ctx);
    }
}

// --- widgets (valid only inside a ui.window build function) ------------------

static int l_label(lua_State* L)
{
    mu_label(need_ctx(L), luaL_checkstring(L, 1));
    return 0;
}

static int l_text(lua_State* L)
{
    mu_text(need_ctx(L), luaL_checkstring(L, 1));
    return 0;
}

static int l_button(lua_State* L)
{
    int res = mu_button(need_ctx(L), luaL_checkstring(L, 1));
    lua_pushboolean(L, res != 0);
    return 1;
}

static int l_header(lua_State* L)
{
    int res = mu_header(need_ctx(L), luaL_checkstring(L, 1));
    lua_pushboolean(L, res != 0);
    return 1;
}

static int l_treenode(lua_State* L)
{
    int res = mu_begin_treenode(need_ctx(L), luaL_checkstring(L, 1));
    lua_pushboolean(L, res != 0);
    return 1;
}

static int l_endtreenode(lua_State* L)
{
    mu_end_treenode(need_ctx(L));
    return 0;
}

static int l_checkbox(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    int state = lua_toboolean(L, 2);
    mu_checkbox(need_ctx(L), label, &state);
    lua_pushboolean(L, state);
    return 1;
}

static int l_slider(lua_State* L)
{
    mu_Real v = (mu_Real)luaL_checknumber(L, 1);
    mu_Real lo = (mu_Real)luaL_checknumber(L, 2);
    mu_Real hi = (mu_Real)luaL_checknumber(L, 3);
    mu_slider(need_ctx(L), &v, lo, hi);
    lua_pushnumber(L, v);
    return 1;
}

// ui.row({w1, w2, ...}, height): set the next row's column widths (0 = default,
// -1 = fill remaining). Height 0 uses the style default.
static int l_row(lua_State* L)
{
    mu_Context* ctx = need_ctx(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    int height = (int)luaL_optinteger(L, 2, 0);
    int widths[MU_MAX_WIDTHS];
    int len = (int)lua_rawlen(L, 1);
    if (len > MU_MAX_WIDTHS) {
        len = MU_MAX_WIDTHS;
    }
    for (int i = 0; i < len; i++) {
        lua_rawgeti(L, 1, i + 1);
        widths[i] = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    mu_layout_row(ctx, len, widths, height);
    return 0;
}

// --- one-shot native dialogs ------------------------------------------------

static int l_popup(lua_State* L)
{
    const char* title = luaL_checkstring(L, 1);
    const char* body = luaL_checkstring(L, 2);
    if (ui_available()) {
        ui_message(title, body);
    }
    return 0;
}

static int l_alert(lua_State* L)
{
    const char* body = luaL_checkstring(L, 1);
    if (ui_available()) {
        ui_message("Alert", body);
    }
    return 0;
}

static int l_confirm(lua_State* L)
{
    const char* body = luaL_checkstring(L, 1);
    bool ok = ui_available() ? ui_confirm("Confirm", body) : true;
    lua_pushboolean(L, ok);
    return 1;
}

static int l_available(lua_State* L)
{
    lua_pushboolean(L, ui_available());
    return 1;
}

// ui.fullscreen(fn): run fn with the desktop suspended, so it can draw to the
// raw framebuffer / use the text console (raytracer, boing, etc.). Without a
// desktop it just calls fn.
static int l_fullscreen(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (!ui_available()) {
        lua_pushvalue(L, 1);
        lua_call(L, 0, 0);
        return 0;
    }
    ui_fullscreen_begin();
    lua_pushvalue(L, 1);
    int st = lua_pcall(L, 0, 0, 0);
    if (st == LUA_OK) {
        // Hold the drawn screen until the user dismisses it, else the desktop
        // would repaint over it the moment we return.
        console_print("\n[press a key to return to the desktop]");
        console_getch();
    }
    ui_fullscreen_end();
    if (st != LUA_OK) {
        return lua_error(L);
    }
    return 0;
}

// --- canvas: an off-screen pixel buffer shown in a window -------------------
// ui.canvas(w,h) -> cv. Draw into it with cv:draw(fn) (fb.* redirected) or
// cv:mem() (raw, for thread.parallel), then cv:show() inside a window build fn.

#define CANVAS_MT "juampi.canvas"
typedef struct {
    uint32_t* buf;
    int w, h;
} LuaCanvas;

static int l_canvas(lua_State* L)
{
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    if (w < 1 || h < 1 || w > 4096 || h > 4096) {
        return luaL_error(L, "ui.canvas: bad size");
    }
    LuaCanvas* cv = (LuaCanvas*)lua_newuserdatauv(L, sizeof(LuaCanvas), 0);
    cv->w = w;
    cv->h = h;
    cv->buf = new (&heap_default()->base, uint32_t, (ptrdiff_t)w * h);
    luaL_setmetatable(L, CANVAS_MT);
    return 1;
}

static LuaCanvas* check_canvas(lua_State* L)
{
    return (LuaCanvas*)luaL_checkudata(L, 1, CANVAS_MT);
}

static int l_canvas_gc(lua_State* L)
{
    LuaCanvas* cv = (LuaCanvas*)luaL_checkudata(L, 1, CANVAS_MT);
    if (cv->buf != NULL) {
        heap_free(heap_default(), cv->buf);
        cv->buf = NULL;
    }
    return 0;
}

// cv:mem() -> memview, pitch, rshift, gshift, bshift (for raw/parallel drawing).
static int l_canvas_mem(lua_State* L)
{
    LuaCanvas* cv = check_canvas(L);
    mem_push_view(L, cv->buf, (size_t)cv->w * (size_t)cv->h * 4);
    lua_pushinteger(L, cv->w * 4);
    uint8_t rs, gs, bs;
    gfx_shifts(&rs, &gs, &bs);
    lua_pushinteger(L, rs);
    lua_pushinteger(L, gs);
    lua_pushinteger(L, bs);
    return 5;
}

// cv:draw(fn) — run fn with gfx (fb.*) redirected into the canvas.
static int l_canvas_draw(lua_State* L)
{
    LuaCanvas* cv = check_canvas(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    gfx_target(cv->buf, (uint64_t)cv->w, (uint64_t)cv->h);
    lua_pushvalue(L, 2);
    int st = lua_pcall(L, 0, 0, 0);
    gfx_target_reset();
    if (st != LUA_OK) {
        return lua_error(L);
    }
    return 0;
}

// cv:show() — blit the canvas into the current window body (build fn only).
static int l_canvas_show(lua_State* L)
{
    LuaCanvas* cv = check_canvas(L);
    mu_Context* ctx = ui_current();
    if (ctx == NULL) {
        return luaL_error(L, "cv:show() called outside a window");
    }
    ui_image(ctx, cv->buf, cv->w, cv->h);
    return 0;
}

static const luaL_Reg canvas_methods[] = {
        {"mem", l_canvas_mem},
        {"draw", l_canvas_draw},
        {"show", l_canvas_show},
        {NULL, NULL},
};

static const luaL_Reg uilib[] = {
        {"window", l_window},           {"open", l_open},
        {"close", l_close},             {"label", l_label},
        {"text", l_text},               {"button", l_button},
        {"header", l_header},           {"treenode", l_treenode},
        {"endtreenode", l_endtreenode}, {"checkbox", l_checkbox},
        {"slider", l_slider},           {"row", l_row},
        {"popup", l_popup},             {"alert", l_alert},
        {"confirm", l_confirm},         {"available", l_available},
        {"fullscreen", l_fullscreen},   {"canvas", l_canvas},
        {NULL, NULL},
};

int luaopen_ui(lua_State* L)
{
    luaL_newlib(L, uilib);
    ui_set_window_hook(build_windows); // desktop renders ui.open() windows

    // Canvas metatable: __gc frees the buffer, __index holds the methods.
    luaL_newmetatable(L, CANVAS_MT);
    lua_pushcfunction(L, l_canvas_gc);
    lua_setfield(L, -2, "__gc");
    luaL_newlib(L, canvas_methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    return 1;
}
