// The `net` library: the vertical slice of networking exposed to Lua — bring-up
// status, our address, and ICMP ping. Backed by the e1000 driver + minimal
// IPv4 stack (src/e1000.c, src/net.c). See docs/networking.md.

#include <net.h>

#include "lua.h"
#include "lauxlib.h"

static const char hexd[] = "0123456789abcdef";

static int l_ready(lua_State* L)
{
    lua_pushboolean(L, net_ready());
    return 1;
}

// net.ip() -> "a.b.c.d" | nil
static int l_ip(lua_State* L)
{
    if (!net_ready()) {
        lua_pushnil(L);
        return 1;
    }
    char buf[16];
    net_ntoa(net_ip(), buf);
    lua_pushstring(L, buf);
    return 1;
}

// net.mac() -> "xx:xx:xx:xx:xx:xx" | nil
static int l_mac(lua_State* L)
{
    if (!net_ready()) {
        lua_pushnil(L);
        return 1;
    }
    uint8_t m[6];
    net_mac(m);
    char buf[18];
    int n = 0;
    for (int i = 0; i < 6; i++) {
        buf[n++] = hexd[m[i] >> 4];
        buf[n++] = hexd[m[i] & 0xF];
        if (i < 5) {
            buf[n++] = ':';
        }
    }
    buf[n] = '\0';
    lua_pushstring(L, buf);
    return 1;
}

// net.config(ip, mask, gateway) — all dotted-quad strings.
static int l_config(lua_State* L)
{
    uint32_t ip, mask, gw;
    if (!net_aton(luaL_checkstring(L, 1), &ip) ||
        !net_aton(luaL_checkstring(L, 2), &mask) ||
        !net_aton(luaL_checkstring(L, 3), &gw)) {
        return luaL_error(L, "net.config: bad address");
    }
    net_config(ip, mask, gw);
    lua_pushboolean(L, 1);
    return 1;
}

// net.ping(host [, timeout_ms=1000]) -> rtt_ms (number) | nil
static int l_ping(lua_State* L)
{
    const char* host = luaL_checkstring(L, 1);
    lua_Integer timeout = luaL_optinteger(L, 2, 1000);
    if (timeout < 1) {
        timeout = 1;
    }
    uint32_t dst;
    if (!net_aton(host, &dst)) {
        return luaL_error(L, "net.ping: expected a dotted-quad IP");
    }
    uint64_t rtt_us = 0;
    if (!net_ping(dst, (uint32_t)timeout, &rtt_us)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, (lua_Number)rtt_us / 1000.0);
    return 1;
}

static const luaL_Reg netlib[] = {
        {"ready", l_ready},   {"ip", l_ip},     {"mac", l_mac},
        {"config", l_config}, {"ping", l_ping}, {NULL, NULL},
};

int luaopen_net(lua_State* L)
{
    luaL_newlib(L, netlib);
    return 1;
}
