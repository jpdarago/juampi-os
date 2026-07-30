// The `net` library exposed to Lua: bring-up status, our address, ICMP ping,
// and UDP sockets. Backed by the e1000 driver + IPv4/ICMP/UDP stack
// (src/e1000.c, src/net.c). See docs/networking.md.

#include <net.h>
#include <e1000.h> // RX interrupt diagnostics (net.rxirqs)
#include <luadoc.h>

#include "lua.h"
#include "lauxlib.h"

#define UDP_MT "juampi.udpsock"
#define TCP_MT "juampi.tcpconn"

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

// net.rxirqs() -> irq_driven, count. Whether NIC receive is interrupt-driven
// and how many RX interrupts have fired (diagnostic; proves the ISR runs).
static int l_rxirqs(lua_State* L)
{
    lua_pushboolean(L, e1000_irq_driven());
    lua_pushinteger(L, (lua_Integer)e1000_irq_count());
    return 2;
}

// net.config(ip, mask, gateway) — all dotted-quad strings.
static int l_config(lua_State* L)
{
    uint32_t ip, mask, gateway_ip;
    if (!net_aton(luaL_checkstring(L, 1), &ip) ||
        !net_aton(luaL_checkstring(L, 2), &mask) ||
        !net_aton(luaL_checkstring(L, 3), &gateway_ip)) {
        return luaL_error(L, "net.config: bad address");
    }
    net_config(ip, mask, gateway_ip);
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
    uint32_t dst_ip;
    if (!net_aton(host, &dst_ip)) {
        return luaL_error(L, "net.ping: expected a dotted-quad IP");
    }
    uint64_t rtt_us = 0;
    if (!net_ping(dst_ip, (uint32_t)timeout, &rtt_us)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, (lua_Number)rtt_us / 1000.0);
    return 1;
}

// net.resolve(host [, timeout_ms=4000]) -> "a.b.c.d" | nil,err
static int l_resolve(lua_State* L)
{
    const char* host = luaL_checkstring(L, 1);
    lua_Integer timeout = luaL_optinteger(L, 2, 4000);
    if (timeout < 1) {
        timeout = 1;
    }
    uint32_t ip;
    if (!net_resolve(host, (uint32_t)timeout, &ip)) {
        lua_pushnil(L);
        lua_pushfstring(L, "could not resolve %s", host);
        return 2;
    }
    char buf[16];
    net_ntoa(ip, buf);
    lua_pushstring(L, buf);
    return 1;
}

// net.dhcp([timeout_ms=4000]) -> "a.b.c.d" (leased address) | nil
static int l_dhcp(lua_State* L)
{
    lua_Integer timeout = luaL_optinteger(L, 1, 4000);
    if (timeout < 1) {
        timeout = 1;
    }
    if (!net_dhcp((uint32_t)timeout)) {
        lua_pushnil(L);
        return 1;
    }
    char buf[16];
    net_ntoa(net_ip(), buf);
    lua_pushstring(L, buf);
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
    uint32_t dst_ip;
    if (!net_aton(luaL_checkstring(L, 2), &dst_ip)) {
        return luaL_error(L, "sendto: expected a dotted-quad IP");
    }
    lua_Integer port = luaL_checkinteger(L, 3);
    size_t len;
    const char* data = luaL_checklstring(L, 4, &len);
    if (*ud < 0 ||
        !net_udp_sendto(*ud, dst_ip, (uint16_t)port, data, (uint16_t)len)) {
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
    uint8_t buf[1472]; // stack-local: copied into Lua before return
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

// --- TCP client -------------------------------------------------------------
// net.connect(ip, port) performs the handshake and returns a connection object.

static int* check_tcp(lua_State* L)
{
    return (int*)luaL_checkudata(L, 1, TCP_MT);
}

// net.connect(ip, port [, timeout_ms=5000]) -> conn | nil, err
static int l_connect(lua_State* L)
{
    uint32_t dst_ip;
    if (!net_aton(luaL_checkstring(L, 1), &dst_ip)) {
        return luaL_error(L, "connect: expected a dotted-quad IP");
    }
    lua_Integer port = luaL_checkinteger(L, 2);
    lua_Integer timeout = luaL_optinteger(L, 3, 5000);
    int id = net_tcp_connect(dst_ip, (uint16_t)port, (uint32_t)timeout);
    if (id < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "connect failed");
        return 2;
    }
    int* ud = (int*)lua_newuserdatauv(L, sizeof(int), 0);
    *ud = id;
    luaL_setmetatable(L, TCP_MT);
    return 1;
}

// net.listen(port) -> listener | nil,err  (a connection object in LISTEN state)
static int l_listen(lua_State* L)
{
    lua_Integer port = luaL_checkinteger(L, 1);
    if (port < 1 || port > 65535) {
        return luaL_error(L, "listen: port out of range");
    }
    int id = net_tcp_listen((uint16_t)port);
    if (id < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "listen failed");
        return 2;
    }
    int* ud = (int*)lua_newuserdatauv(L, sizeof(int), 0);
    *ud = id;
    luaL_setmetatable(L, TCP_MT);
    return 1;
}

// listener:accept([timeout_ms=20000]) -> true | nil,err. On success the same
// object is now a connected socket (single connection per listener).
static int l_tcp_accept(lua_State* L)
{
    int* ud = check_tcp(L);
    lua_Integer timeout = luaL_optinteger(L, 2, 20000);
    if (*ud < 0 || net_tcp_accept(*ud, (uint32_t)timeout) < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "accept timeout");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// conn:send(data) -> bytes | nil,err
static int l_tcp_send(lua_State* L)
{
    int* ud = check_tcp(L);
    size_t len;
    const char* data = luaL_checklstring(L, 2, &len);
    int n = (*ud < 0) ? -1 : net_tcp_send(*ud, data, (uint32_t)len);
    if (n < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "send failed");
        return 2;
    }
    lua_pushinteger(L, n);
    return 1;
}

// conn:recv([cap=4096 [, timeout_ms=5000]]) -> data | nil, "closed"|"timeout"
static int l_tcp_recv(lua_State* L)
{
    int* ud = check_tcp(L);
    lua_Integer cap = luaL_optinteger(L, 2, 4096);
    lua_Integer timeout = luaL_optinteger(L, 3, 5000);
    uint8_t buf[4096]; // stack-local: copied into Lua before return
    if (cap < 1) {
        cap = 1;
    }
    if (cap > (lua_Integer)sizeof(buf)) {
        cap = sizeof(buf);
    }
    int n = (*ud < 0)
                    ? -1
                    : net_tcp_recv(*ud, buf, (uint32_t)cap, (uint32_t)timeout);
    if (n > 0) {
        lua_pushlstring(L, (const char*)buf, (size_t)n);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, n == 0 ? "closed" : "timeout");
    return 2;
}

static int l_tcp_close(lua_State* L)
{
    int* ud = check_tcp(L);
    if (*ud >= 0) {
        net_tcp_close(*ud);
        *ud = -1;
    }
    return 0;
}

static const luaL_Reg tcp_methods[] = {
        {"accept", l_tcp_accept}, {"send", l_tcp_send}, {"recv", l_tcp_recv},
        {"close", l_tcp_close},   {NULL, NULL},
};

static const struct lua_fndoc netlib[] = {
        {"ready", l_ready, "Whether the network stack has an IP address.",
         .rets = {{"ok", "boolean", "true if configured"}}},
        {"ip", l_ip, "The current IPv4 address.",
         .rets = {{"addr", "string?", "dotted-quad, or nil if unconfigured"}}},
        {"mac", l_mac, "The NIC's MAC address.",
         .rets = {{"mac", "string?", "xx:xx:xx:xx:xx:xx, or nil"}}},
        {"rxirqs", l_rxirqs, "NIC receive-interrupt state (diagnostic).",
         .rets = {{"irq", "boolean", "true if RX is interrupt-driven"},
                  {"count", "number", "receive interrupts taken"}}},
        {"config", l_config, "Set a static IPv4 configuration.",
         .args = {{"ip", "string", "dotted-quad address"},
                  {"mask", "string", "dotted-quad netmask"},
                  {"gateway", "string", "dotted-quad gateway"}}},
        {"dhcp", l_dhcp, "Acquire an address by DHCP.",
         .args = {{"timeout_ms", "number?", "timeout, default 4000"}},
         .rets = {{"addr", "string?", "leased address, or nil on failure"}}},
        {"resolve", l_resolve, "Resolve a hostname to an IPv4 address (DNS).",
         .args = {{"host", "string", "hostname"},
                  {"timeout_ms", "number?", "timeout, default 4000"}},
         .rets = {{"addr", "string?", "dotted-quad, or nil on error"},
                  {"err", "string?", "error message when addr is nil"}}},
        {"ping", l_ping, "ICMP echo a host and time the round trip.",
         .args = {{"host", "string", "IP or hostname"},
                  {"timeout_ms", "number?", "timeout, default 1000"}},
         .rets = {{"rtt_ms", "number?", "round-trip ms, or nil on timeout"}}},
        {"udp", l_udp, "Create a UDP socket.",
         .rets = {{"sock", "userdata?",
                   "socket with :bind/:sendto/:recvfrom/:close, or nil"},
                  {"err", "string?", "error message when sock is nil"}}},
        {"connect", l_connect, "Open a TCP connection (active).",
         .args = {{"ip", "string", "dotted-quad address"},
                  {"port", "number", "destination port"},
                  {"timeout_ms", "number?", "handshake timeout, default 5000"}},
         .rets = {{"conn", "userdata?",
                   "connection with :send/:recv/:close, or nil"},
                  {"err", "string?", "error message when conn is nil"}}},
        {"listen", l_listen, "Open a TCP listener on a port.",
         .args = {{"port", "number", "local port to listen on"}},
         .rets = {{"listener", "userdata?", "listener with :accept, or nil"},
                  {"err", "string?", "error message when listener is nil"}}},
        {0},
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
    register_obj(L, TCP_MT, tcp_methods, l_tcp_close);
    luadoc_newlib(L, netlib);
    return 1;
}
