// The `fb` library: draw to the framebuffer from Lua. Colours are 0xRRGGBB.
// Shares the screen with the text console, so drawing overwrites text and vice
// versa. Good for visualizing what the `k` library measures.

#include <gfx.h>
#include <qoi.h>
#include <kmodule.h>
#include <memory.h>
#include <parallel.h> // mem_push_view, for fb.canvas

#include "lua.h"
#include "lauxlib.h"

static int l_width(lua_State* L)
{
    lua_pushinteger(L, gfx_width());
    return 1;
}
static int l_height(lua_State* L)
{
    lua_pushinteger(L, gfx_height());
    return 1;
}
static int l_pixel(lua_State* L)
{
    gfx_pixel(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2),
              (uint32_t)luaL_checkinteger(L, 3));
    return 0;
}
static int l_rect(lua_State* L)
{
    gfx_rect(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2),
             luaL_checkinteger(L, 3), luaL_checkinteger(L, 4),
             (uint32_t)luaL_checkinteger(L, 5));
    return 0;
}
static int l_clear(lua_State* L)
{
    gfx_clear((uint32_t)luaL_optinteger(L, 1, 0));
    return 0;
}
static int l_line(lua_State* L)
{
    gfx_line(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2),
             luaL_checkinteger(L, 3), luaL_checkinteger(L, 4),
             (uint32_t)luaL_checkinteger(L, 5));
    return 0;
}

// fb.buffer([on]) -> bool. Turn double buffering on (default) or off, returning
// whether it is now active. While on, drawing goes to an off-screen buffer and
// nothing appears until fb.flip(), so animations don't flicker.
static int l_buffer(lua_State* L)
{
    bool on = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1);
    lua_pushboolean(L, gfx_buffer(on));
    return 1;
}

// fb.flip(). Copy the back buffer to the screen in one pass. No-op unless
// double buffering is on.
static int l_flip(lua_State* L)
{
    (void)L;
    gfx_flip();
    return 0;
}

// fb.image(name [,x,y [,key [,tol]]]) -> width, height. Decode a QOI image
// shipped as a Limine module and blit it. x/y default to centring the image on
// screen. If `key` (an 0xRRGGBB colour) is given, pixels within `tol` (default
// 16) of it per channel are made transparent — a chroma key, so e.g. a solid
// background colour drops out. (Images can also carry their own alpha.)
static int l_image(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    size_t size = 0;
    const void* data = kmodule_find(name, &size);
    if (data == NULL) {
        return luaL_error(L, "no such image: %s", name);
    }
    qoi_image img;
    uint32_t* pixels = qoi_decode(&heap_default()->base, data, size, &img);
    if (pixels == NULL) {
        return luaL_error(L, "not a valid QOI image: %s", name);
    }

    if (!lua_isnoneornil(L, 4)) {
        uint32_t key = (uint32_t)luaL_checkinteger(L, 4);
        int tol = (int)luaL_optinteger(L, 5, 16);
        int kr = (key >> 16) & 0xff, kg = (key >> 8) & 0xff, kb = key & 0xff;
        uint64_t n = (uint64_t)img.width * img.height;
        for (uint64_t i = 0; i < n; i++) {
            int r = (pixels[i] >> 16) & 0xff;
            int g = (pixels[i] >> 8) & 0xff;
            int b = pixels[i] & 0xff;
            int dr = r - kr, dg = g - kg, db = b - kb;
            if (dr < 0) {
                dr = -dr;
            }
            if (dg < 0) {
                dg = -dg;
            }
            if (db < 0) {
                db = -db;
            }
            if (dr <= tol && dg <= tol && db <= tol) {
                pixels[i] = 0; // alpha 0 -> skipped by gfx_blit
            }
        }
    }

    int64_t x = luaL_optinteger(
            L, 2, ((int64_t)gfx_width() - (int64_t)img.width) / 2);
    int64_t y = luaL_optinteger(
            L, 3, ((int64_t)gfx_height() - (int64_t)img.height) / 2);
    gfx_blit(x, y, img.width, img.height, pixels);
    heap_free(heap_default(), pixels);
    lua_pushinteger(L, img.width);
    lua_pushinteger(L, img.height);
    return 2;
}

// fb.setmode(w, h) -> bool. Switch to a w*h 32bpp mode at runtime.
static int l_setmode(lua_State* L)
{
    lua_pushboolean(L, gfx_set_mode((uint32_t)luaL_checkinteger(L, 1),
                                    (uint32_t)luaL_checkinteger(L, 2)));
    return 1;
}

// fb.pitch() -> bytes per scanline.
static int l_pitch(lua_State* L)
{
    lua_pushinteger(L, gfx_pitch());
    return 1;
}

// fb.shifts() -> r, g, b bit shifts for the current mode (for packing pixels
// written directly to fb.canvas).
static int l_shifts(lua_State* L)
{
    uint8_t r, g, b;
    gfx_shifts(&r, &g, &b);
    lua_pushinteger(L, r);
    lua_pushinteger(L, g);
    lua_pushinteger(L, b);
    return 3;
}

// fb.canvas() -> a shared buffer aliasing the live framebuffer, so pixels can be
// written directly (e.g. by every core in a parallel renderer). Index with
// y*fb.pitch() + x*4 and store a packed pixel via :u32().
static int l_canvas(lua_State* L)
{
    uint64_t size = 0, pitch = 0;
    void* fb = gfx_framebuffer(&size, &pitch);
    if (fb == NULL) {
        return luaL_error(L, "fb.canvas: no framebuffer");
    }
    mem_push_view(L, fb, (size_t)size);
    return 1;
}

static const luaL_Reg fblib[] = {
        {"width", l_width},     {"height", l_height}, {"pixel", l_pixel},
        {"rect", l_rect},       {"clear", l_clear},   {"line", l_line},
        {"image", l_image},     {"buffer", l_buffer}, {"flip", l_flip},
        {"setmode", l_setmode}, {"pitch", l_pitch},   {"shifts", l_shifts},
        {"canvas", l_canvas},   {NULL, NULL},
};

int luaopen_fb(lua_State* L)
{
    luaL_newlib(L, fblib);
    return 1;
}
