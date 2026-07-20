// The `lab` library: load native ELF64 benchmark binaries and run them in
// ring 0 (see lab.h / src/lab.c). The counterpart to run() for compiled C — a
// "sterile lab" to benchmark algorithm implementations against each other.

#include <lab.h>
#include <kmodule.h>
#include <ext2.h>
#include <memory.h>

#include <printf/printf.h>

#include "lua.h"
#include "lauxlib.h"

// Resolve a binary by name: a built-in Limine module first, then the ext2 disk
// (name, /name, /lab/name) — the same order run() uses for scripts. On an ext2
// hit *owned is the heap buffer the caller must free.
static const void* resolve(const char* name, size_t* size, void** owned)
{
    *owned = NULL;
    const void* data = kmodule_find(name, size);
    if (data != NULL) {
        return data;
    }
    void* d = ext2_read_path(name, size);
    if (d == NULL && name[0] != '/') {
        char path[256];
        snprintf(path, sizeof path, "/%s", name);
        d = ext2_read_path(path, size);
        if (d == NULL) {
            snprintf(path, sizeof path, "/lab/%s", name);
            d = ext2_read_path(path, size);
        }
    }
    *owned = d;
    return d;
}

// lab.run(name [,arg=0]) -> result. Load the binary and call its entry once.
static int l_run(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    long arg = (long)luaL_optinteger(L, 2, 0);
    size_t size = 0;
    void* owned = NULL;
    const void* data = resolve(name, &size, &owned);
    if (data == NULL) {
        return luaL_error(L, "no such lab binary: %s", name);
    }
    long r = lab_run(data, size, arg);
    if (owned != NULL) {
        heap_free(heap_default(), owned);
    }
    lua_pushinteger(L, (lua_Integer)r);
    return 1;
}

// lab.bench(name [,arg=0 [,iters=1000]]) -> total_cycles, cycles_per_call.
static int l_bench(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    long arg = (long)luaL_optinteger(L, 2, 0);
    lua_Integer iters = luaL_optinteger(L, 3, 1000);
    if (iters < 1) {
        iters = 1;
    }
    size_t size = 0;
    void* owned = NULL;
    const void* data = resolve(name, &size, &owned);
    if (data == NULL) {
        return luaL_error(L, "no such lab binary: %s", name);
    }
    unsigned long cycles = lab_bench(data, size, arg, (unsigned long)iters);
    if (owned != NULL) {
        heap_free(heap_default(), owned);
    }
    lua_pushinteger(L, (lua_Integer)cycles);
    lua_pushnumber(L, (lua_Number)cycles / (lua_Number)iters);
    return 2;
}

static const luaL_Reg lablib[] = {
        {"run", l_run},
        {"bench", l_bench},
        {NULL, NULL},
};

int luaopen_lab(lua_State* L)
{
    luaL_newlib(L, lablib);
    return 1;
}
