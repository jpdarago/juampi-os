// Glue between the kernel shell and the embedded Lua interpreter. Compiled with
// the Lua include path (so its libc stubs resolve), it holds one persistent
// lua_State and evaluates a line at a time, printing results like the standard
// Lua REPL (an expression prints its value).

#include <luashell.h>
#include <console.h>

#include <printf/printf.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static lua_State* L;

void luashell_init(void)
{
    L = luaL_newstate();
    if (L == NULL) {
        console_print("lua: could not create interpreter state\n");
        return;
    }
    // Open a curated set of libraries (no io/os/package: this is a ring-0
    // freestanding interpreter).
    static const luaL_Reg libs[] = {
            {LUA_GNAME, luaopen_base},        {LUA_TABLIBNAME, luaopen_table},
            {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
            {LUA_COLIBNAME, luaopen_coroutine},
    };
    for (unsigned i = 0; i < sizeof(libs) / sizeof(libs[0]); i++) {
        luaL_requiref(L, libs[i].name, libs[i].func, 1);
        lua_pop(L, 1);
    }
}

// Print every value from stack index `from` to the top, tab-separated, using
// Lua's tostring conversion.
static void print_results(int from)
{
    int top = lua_gettop(L);
    for (int i = from; i <= top; i++) {
        const char* s = luaL_tolstring(L, i, NULL); // pushes the string
        console_print(s);
        console_print(i < top ? "\t" : "\n");
        lua_pop(L, 1);
    }
}

void luashell_eval(const char* line)
{
    if (L == NULL) {
        return;
    }
    // Try to compile "return <line>" first, so a bare expression prints its
    // value; if that fails to compile, run the line as a statement.
    char buf[600];
    snprintf(buf, sizeof(buf), "return %s", line);
    int status = luaL_loadstring(L, buf);
    if (status != LUA_OK) {
        lua_pop(L, 1);
        status = luaL_loadstring(L, line);
    }

    if (status == LUA_OK) {
        int base = lua_gettop(L); // the compiled chunk
        status = lua_pcall(L, 0, LUA_MULTRET, 0);
        if (status == LUA_OK) {
            print_results(base);
            lua_settop(L, base - 1);
            return;
        }
    }

    // Compile or runtime error: report the message on the top of the stack.
    const char* msg = lua_tostring(L, -1);
    console_print(msg != NULL ? msg : "(unknown error)");
    console_print("\n");
    lua_settop(L, 0);
}
