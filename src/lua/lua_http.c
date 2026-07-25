// The `http` library: a plaintext HTTP/1.1 client (http.get). Backed by src/http.c
// over the TCP stack + the vendored picohttpparser. https:// is a later round.

#include <http.h>

#include "lua.h"
#include "lauxlib.h"

// http.get(url) -> status:int, body:string | nil, errmsg
static int l_get(lua_State* L)
{
    const char* url = luaL_checkstring(L, 1);
    // Sized for typical pages; larger bodies are truncated to this.
    static char body[256 * 1024];
    int blen = 0;
    int status = http_get(url, body, (int)sizeof(body), &blen);
    if (status < 0) {
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
    lua_pushlstring(L, body, (size_t)blen);
    return 2;
}

static const luaL_Reg httplib[] = {
        {"get", l_get},
        {NULL, NULL},
};

int luaopen_http(lua_State* L)
{
    luaL_newlib(L, httplib);
    return 1;
}
