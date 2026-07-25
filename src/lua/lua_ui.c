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

#include "lua.h"
#include "lauxlib.h"

#include "../microui/microui.h"

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

static const luaL_Reg uilib[] = {
        {"window", l_window},         {"label", l_label},
        {"text", l_text},             {"button", l_button},
        {"header", l_header},         {"treenode", l_treenode},
        {"endtreenode", l_endtreenode}, {"checkbox", l_checkbox},
        {"slider", l_slider},         {"row", l_row},
        {"popup", l_popup},           {"alert", l_alert},
        {"confirm", l_confirm},       {"available", l_available},
        {NULL, NULL},
};

int luaopen_ui(lua_State* L)
{
    luaL_newlib(L, uilib);
    return 1;
}
