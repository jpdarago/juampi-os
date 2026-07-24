// The `net` library exposed to Lua: bring-up status, our address, ICMP ping,
// and UDP sockets. Backed by the e1000 driver + IPv4/ICMP/UDP stack
// (src/e1000.c, src/net.c). See docs/networking.md.

#include <net.h>

#include "lua.h"
#include "lauxlib.h"

#define UDP_MT "juampi.udpsock"

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

// --- UDP sockets ------------------------------------------------------------
// net.udp() returns a socket object; the handle is stored in a userdata so a
// __gc can release it if the script drops the reference without closing.

static int* check_udp(lua_State* L)
{
    return (int*)luaL_checkudata(L, 1, UDP_MT);
}

// net.udp() -> socket | nil, err
static int l_udp(lua_State* L)
{
    int s = net_udp_open();
    if (s < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no free udp sockets");
        return 2;
    }
    int* ud = (int*)lua_newuserdatauv(L, sizeof(int), 0);
    *ud = s;
    luaL_setmetatable(L, UDP_MT);
    return 1;
}

// sock:bind([port=0]) -> true | nil,err   (0 = ephemeral)
static int l_udp_bind(lua_State* L)
{
    int* ud = check_udp(L);
    lua_Integer port = luaL_optinteger(L, 2, 0);
    if (port < 0 || port > 65535) {
        return luaL_error(L, "bind: port out of range");
    }
    if (*ud < 0 || !net_udp_bind(*ud, (uint16_t)port)) {
        lua_pushnil(L);
        lua_pushstring(L, "bind failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// sock:sendto(ip, port, data) -> true | nil,err
static int l_udp_sendto(lua_State* L)
{
    int* ud = check_udp(L);
    uint32_t dst;
    if (!net_aton(luaL_checkstring(L, 2), &dst)) {
        return luaL_error(L, "sendto: expected a dotted-quad IP");
    }
    lua_Integer port = luaL_checkinteger(L, 3);
    size_t len;
    const char* data = luaL_checklstring(L, 4, &len);
    if (*ud < 0 ||
        !net_udp_sendto(*ud, dst, (uint16_t)port, data, (uint16_t)len)) {
        lua_pushnil(L);
        lua_pushstring(L, "send failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// sock:recvfrom([timeout_ms=1000]) -> data, ip, port | nil (timeout)
static int l_udp_recvfrom(lua_State* L)
{
    int* ud = check_udp(L);
    lua_Integer timeout = luaL_optinteger(L, 2, 1000);
    if (timeout < 0) {
        timeout = 0;
    }
    static uint8_t buf[1472];
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    int n = (*ud < 0) ? -1
                      : net_udp_recvfrom(*ud, (uint32_t)timeout, buf,
                                         sizeof(buf), &src_ip, &src_port);
    if (n < 0) {
        lua_pushnil(L);
        return 1;
    }
    size_t got = (size_t)n > sizeof(buf) ? sizeof(buf) : (size_t)n;
    lua_pushlstring(L, (const char*)buf, got);
    char ips[16];
    net_ntoa(src_ip, ips);
    lua_pushstring(L, ips);
    lua_pushinteger(L, src_port);
    return 3;
}

// sock:close()
static int l_udp_close(lua_State* L)
{
    int* ud = check_udp(L);
    if (*ud >= 0) {
        net_udp_close(*ud);
        *ud = -1;
    }
    return 0;
}

static const luaL_Reg udp_methods[] = {
        {"bind", l_udp_bind},
        {"sendto", l_udp_sendto},
        {"recvfrom", l_udp_recvfrom},
        {"close", l_udp_close},
        {NULL, NULL},
};

static const luaL_Reg netlib[] = {
        {"ready", l_ready},   {"ip", l_ip},     {"mac", l_mac},
        {"config", l_config}, {"ping", l_ping}, {"udp", l_udp},
        {NULL, NULL},
};

// Register a metatable for an object type: a method table under __index and a
// __gc finalizer on the metatable itself.
static void register_obj(lua_State* L, const char* mt, const luaL_Reg* methods,
                         lua_CFunction gc)
{
    if (luaL_newmetatable(L, mt)) {
        lua_newtable(L);
        luaL_setfuncs(L, methods, 0);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

int luaopen_net(lua_State* L)
{
    register_obj(L, UDP_MT, udp_methods, l_udp_close);
    luaL_newlib(L, netlib);
    return 1;
}
