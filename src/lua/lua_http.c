// The `http` library: a plaintext HTTP/1.1 client (http.get). Backed by src/http.c
// over the TCP stack + the vendored picohttpparser. https:// is a later round.

#include <http.h>
#include <arena.h>
#include <memory.h>
#include <luadoc.h>

#include "lua.h"
#include "lauxlib.h"

// http.get(url) -> status:int, body:string | nil, errmsg
static int l_get(lua_State* L)
{
    const char* url = luaL_checkstring(L, 1);

    // Per-call scratch carved from the kernel heap and wrapped in an arena for
    // http_get's response buffer (and, for https, the ~tens-of-KB BearSSL TLS
    // context) — nothing static, so this stays reentrant / safe if the caller
    // ever runs off the BSP. Freed once the body is copied into a Lua string.
    ptrdiff_t sz = HTTP_RECV_MAX + 128 * 1024;
    void* mem = alloc(&heap_default()->base, sz, 16, 1);
    struct arena scratch = arena_init(mem, sz);

    char* body = NULL;
    int blen = 0;
    int status = http_get(&scratch.base, url, &body, &blen);
    if (status < 0) {
        heap_free(heap_default(), mem);
        const char* msg;
        switch (status) {
        case -1:
            msg = "malformed URL (expected http://...)";
            break;
        case -2:
            msg = "DNS resolution failed";
            break;
        case -3:
            msg = "connection failed";
            break;
        default:
            msg = "no/unparseable response";
            break;
        }
        lua_pushnil(L);
        lua_pushstring(L, msg);
        return 2;
    }
    lua_pushinteger(L, status);
    lua_pushlstring(L, body, (size_t)blen); // copies into a Lua string
    heap_free(heap_default(), mem);          // safe: the copy is done
    return 2;
}

static const struct lua_fndoc httplib[] = {
        {"get", l_get, "Fetch a URL over HTTP/1.1.",
         .args = {{"url", "string", "http:// URL"}},
         .rets = {{"status", "number?", "HTTP status code, or nil on error"},
                  {"body", "string?", "response body, or the error message"}}},
        {0},
};

int luaopen_http(lua_State* L)
{
    luadoc_newlib(L, httplib);
    return 1;
}
