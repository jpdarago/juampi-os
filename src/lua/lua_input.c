// The `input` library: non-blocking keyboard and mouse for Lua programs that
// own the screen (framebuffer games/demos), as opposed to the microui `ui`
// windows or the REPL's line reader. Both draw from the same PS/2/USB-HID input
// the kernel already decodes: keyboard_poll() drains the key ring, mouse_poll()
// drains accumulated movement. Callers typically poll these inside their own
// draw loop, with k.sleep() pacing the frames.

#include <keyboard.h>
#include <mouse.h>
#include <gfx.h> // gfx_width/height, to clamp the cursor to the screen

#include <luadoc.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

// input.key() -> the next pending character as a 1-byte string, or nil if the
// key ring is empty. Non-blocking. Arrow keys arrive as an ESC-[ sequence (so
// several successive calls); ordinary keys are one byte each.
static int l_key(lua_State* L)
{
    int c = keyboard_poll();
    if (c < 0) {
        lua_pushnil(L);
        return 1;
    }
    char ch = (char)c;
    lua_pushlstring(L, &ch, 1);
    return 1;
}

// input.mouse() -> x, y, buttons, present. x/y are an absolute cursor position
// (in screen pixels) this library accumulates from the relative movement the
// driver reports, clamped to the screen. `buttons` is a bitmask (1 left, 2
// right, 4 middle); `present` is false if no mouse was detected.
static int l_mouse(lua_State* L)
{
    static bool inited;
    static int x, y;

    int64_t w = (int64_t)gfx_width();
    int64_t h = (int64_t)gfx_height();
    if (!inited) {
        x = (int)(w / 2);
        y = (int)(h / 2);
        inited = true;
    }

    int dx = 0, dy = 0;
    uint8_t buttons = 0;
    bool present = mouse_poll(&dx, &dy, &buttons);
    x += dx;
    y += dy;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (w > 0 && x >= (int)w) {
        x = (int)w - 1;
    }
    if (h > 0 && y >= (int)h) {
        y = (int)h - 1;
    }

    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    lua_pushinteger(L, buttons);
    lua_pushboolean(L, present);
    return 4;
}

static const struct lua_fndoc inputlib[] = {
        {"key", l_key, "Next pending key as a 1-byte string, or nil (non-blocking).",
         .rets = {{"ch", "string?", "the character, or nil if none pending"}}},
        {"mouse", l_mouse, "Absolute cursor position and button state.",
         .rets = {{"x", "number", "cursor x in screen pixels"},
                  {"y", "number", "cursor y in screen pixels"},
                  {"buttons", "number", "bitmask: 1 left, 2 right, 4 middle"},
                  {"present", "boolean", "false if no mouse was detected"}}},
        {0},
};

int luaopen_input(lua_State* L)
{
    luadoc_newlib(L, inputlib);
    return 1;
}
