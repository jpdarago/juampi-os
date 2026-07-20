// Glue between the kernel shell and the embedded Lua interpreter. Compiled with
// the Lua include path (so its libc stubs resolve), it holds one persistent
// lua_State and evaluates input a line at a time like the standard Lua REPL: a
// bare expression prints its value, and incomplete input asks for more.

#include <luashell.h>
#include <console.h>
#include <kmodule.h>
#include <ext2.h>
#include <memory.h>

#include <printf/printf.h>
#include <string.h>
#include <stdbool.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

// The kernel libraries (lua_k.c, lua_fb.c, lua_pci.c, lua_disk.c, lua_fs.c).
int luaopen_k(lua_State* L);
int luaopen_fb(lua_State* L);
int luaopen_pci(lua_State* L);
int luaopen_disk(lua_State* L);
int luaopen_fs(lua_State* L);

static lua_State* L;

// Accumulator for multi-line input (pasted or continued scripts).
#define PENDING_MAX 4096
static char pending[PENDING_MAX];
static size_t pending_len;

// run("name.lua"): load a script and execute it. Built-in scripts shipped as
// Limine modules win; otherwise the ext2 data disk is searched, trying the name
// as given and then rooted at / and /scripts/.
static int l_run(lua_State* Ls)
{
    const char* name = luaL_checkstring(Ls, 1);
    size_t size = 0;
    const void* data = kmodule_find(name, &size);
    void* owned = NULL; // heap buffer when loaded from disk (must be freed)
    if (data == NULL) {
        owned = ext2_read_path(name, &size);
        if (owned == NULL && name[0] != '/') {
            char path[256];
            snprintf(path, sizeof path, "/%s", name);
            owned = ext2_read_path(path, &size);
            if (owned == NULL) {
                snprintf(path, sizeof path, "/scripts/%s", name);
                owned = ext2_read_path(path, &size);
            }
        }
        data = owned;
    }
    if (data == NULL) {
        return luaL_error(Ls, "no such script: %s", name);
    }
    int base = lua_gettop(Ls);
    int status = luaL_loadbuffer(Ls, data, size, name);
    if (owned != NULL) {
        heap_free(heap_default(), owned); // loadbuffer copied the bytes
    }
    if (status != LUA_OK) {
        return lua_error(Ls);
    }
    lua_call(Ls, 0, LUA_MULTRET);
    return lua_gettop(Ls) - base;
}

// Run init.lua (if shipped) once at startup.
static void run_init(void)
{
    size_t size = 0;
    const void* data = kmodule_find("init.lua", &size);
    if (data == NULL) {
        return;
    }
    if (luaL_loadbuffer(L, data, size, "@init.lua") == LUA_OK &&
        lua_pcall(L, 0, 0, 0) == LUA_OK) {
        return;
    }
    console_print("init.lua: ");
    console_print(lua_tostring(L, -1));
    console_print("\n");
    lua_pop(L, 1);
}

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
            {"k", luaopen_k},       // kernel introspection
            {"fb", luaopen_fb},     // framebuffer drawing
            {"pci", luaopen_pci},   // PCI configuration space
            {"disk", luaopen_disk}, // raw ATA block access
            {"fs", luaopen_fs},     // read-only ext2 filesystem
    };
    for (unsigned i = 0; i < sizeof(libs) / sizeof(libs[0]); i++) {
        luaL_requiref(L, libs[i].name, libs[i].func, 1);
        lua_pop(L, 1);
    }
    lua_pushcfunction(L, l_run);
    lua_setglobal(L, "run");

    // Clear any half-entered input (e.g. when re-initializing after a recovered
    // fault longjmp'd out mid-evaluation).
    pending_len = 0;

    run_init();
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

// A LUA_ERRSYNTAX whose message ends with "<eof>" means the chunk is incomplete
// (more input needed) rather than malformed.
static bool is_incomplete(int status)
{
    if (status != LUA_ERRSYNTAX) {
        return false;
    }
    size_t len = 0;
    const char* msg = lua_tolstring(L, -1, &len);
    const char* mark = "<eof>";
    size_t ml = 5;
    return len >= ml && memcmp(msg + len - ml, mark, ml) == 0;
}

int luashell_eval(const char* line)
{
    if (L == NULL) {
        return 0;
    }
    // Append this line to any pending (continued) input.
    size_t ll = strlen(line);
    if (pending_len + ll + 2 >= PENDING_MAX) {
        console_print("input too long\n");
        pending_len = 0;
        return 0;
    }
    if (pending_len > 0) {
        pending[pending_len++] = '\n';
    }
    memcpy(pending + pending_len, line, ll);
    pending_len += ll;
    pending[pending_len] = '\0';

    // Try "return <src>" first so a bare expression prints its value.
    char buf[PENDING_MAX + 8];
    snprintf(buf, sizeof(buf), "return %s", pending);
    int status = luaL_loadstring(L, buf);
    if (status != LUA_OK) {
        lua_pop(L, 1);
        status = luaL_loadstring(L, pending);
        if (is_incomplete(status)) {
            lua_pop(L, 1);
            return 1; // ask the shell for another line
        }
    }

    if (status == LUA_OK) {
        int base = lua_gettop(L);
        status = lua_pcall(L, 0, LUA_MULTRET, 0);
        if (status == LUA_OK) {
            print_results(base);
            lua_settop(L, base - 1);
            pending_len = 0;
            return 0;
        }
    }

    // Compile or runtime error: report the message on the top of the stack.
    const char* msg = lua_tostring(L, -1);
    console_print(msg != NULL ? msg : "(unknown error)");
    console_print("\n");
    lua_settop(L, 0);
    pending_len = 0;
    return 0;
}
