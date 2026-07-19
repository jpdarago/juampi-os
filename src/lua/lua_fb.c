// The `fb` library: draw to the framebuffer from Lua. Colours are 0xRRGGBB.
// Shares the screen with the text console, so drawing overwrites text and vice
// versa. Good for visualizing what the `k` library measures.

#include <gfx.h>

#include "lua.h"
#include "lauxlib.h"

static int l_width(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)gfx_width());
    return 1;
}
static int l_height(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)gfx_height());
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

static const luaL_Reg fblib[] = {
        {"width", l_width}, {"height", l_height}, {"pixel", l_pixel},
        {"rect", l_rect},   {"clear", l_clear},   {"line", l_line},
        {NULL, NULL},
};

int luaopen_fb(lua_State* L)
{
    luaL_newlib(L, fblib);
    return 1;
}
