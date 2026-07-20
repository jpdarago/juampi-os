// The unified launch/benchmark surface: run() and bench(), each polymorphic
// over Lua scripts and native ELF64 binaries (the "lab"). One name resolver, one
// benchmark harness — so an algorithm's Lua and C implementations can be
// launched with the same verb and compared head to head.
//
//   run(name [,arg])                     -> run a .lua script or an ELF binary
//   bench(target [,arg=0] [,iters=1000]) -> total_cycles, cycles_per_call
//     target is a function, a script name, or a binary name.

#include <lab.h>
#include <kmodule.h>
#include <ext2.h>
#include <memory.h>
#include <ktime.h>

#include <printf/printf.h>
#include <stdint.h>
#include <stddef.h>

#include "lua.h"
#include "lauxlib.h"

// Resolve `name` to its bytes: a built-in Limine module first, then the ext2
// disk (name, /name, /scripts/name, /lab/name). On an ext2 hit, *owned is the
// heap buffer the caller must free; for a module it is NULL.
static const void* artifact_load(const char* name, size_t* size, void** owned)
{
    *owned = NULL;
    const void* data = kmodule_find(name, size);
    if (data != NULL) {
        return data;
    }
    void* d = ext2_read_path(name, size);
    if (d == NULL && name[0] != '/') {
        char path[256];
        snprintf(path, sizeof(path), "/%s", name);
        d = ext2_read_path(path, size);
        if (d == NULL) {
            snprintf(path, sizeof(path), "/scripts/%s", name);
            d = ext2_read_path(path, size);
        }
        if (d == NULL) {
            snprintf(path, sizeof(path), "/lab/%s", name);
            d = ext2_read_path(path, size);
        }
    }
    *owned = d;
    return d;
}

static int is_elf(const void* data, size_t size)
{
    const unsigned char* b = data;
    return size >= 4 && b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' &&
           b[3] == 'F';
}

// Time `iters` calls of the Lua callable at absolute stack index `idx`, each
// passed `arg`; return the elapsed TSC cycles. (The callable stays on the stack.)
static uint64_t time_lua_callable(lua_State* L, int idx, long arg,
                                  uint64_t iters)
{
    uint64_t start = rdtsc();
    for (uint64_t i = 0; i < iters; i++) {
        lua_pushvalue(L, idx);
        lua_pushinteger(L, (lua_Integer)arg);
        lua_call(L, 1, 0);
    }
    return rdtsc() - start;
}

// run(name [,arg]): dispatch by artifact type. An ELF is loaded and called in
// ring 0 (returning its result); anything else is loaded and executed as a Lua
// chunk (receiving `arg` as a vararg, returning its own values as before).
static int l_run(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    size_t size = 0;
    void* owned = NULL;
    const void* data = artifact_load(name, &size, &owned);
    if (data == NULL) {
        return luaL_error(L, "no such artifact: %s", name);
    }

    if (is_elf(data, size)) {
        long arg = (long)luaL_optinteger(L, 2, 0);
        long r = lab_run(data, size, arg);
        if (owned != NULL) {
            heap_free(heap_default(), owned);
        }
        lua_pushinteger(L, (lua_Integer)r);
        return 1;
    }

    int base = lua_gettop(L);
    int status = luaL_loadbuffer(L, data, size, name);
    if (owned != NULL) {
        heap_free(heap_default(), owned); // loadbuffer copied the bytes
    }
    if (status != LUA_OK) {
        return lua_error(L);
    }
    int nargs = 0;
    if (!lua_isnoneornil(L, 2)) {
        lua_pushvalue(L, 2);
        nargs = 1;
    }
    lua_call(L, nargs, LUA_MULTRET);
    return lua_gettop(L) - base;
}

// bench(target [,arg=0] [,iters=1000]) -> total_cycles, cycles_per_call.
// target may be a function, a Lua script name, or a native binary name; all
// three are timed the same way so the results are directly comparable.
static int l_bench(lua_State* L)
{
    long arg = (long)luaL_optinteger(L, 2, 0);
    lua_Integer iters = luaL_optinteger(L, 3, 1000);
    if (iters < 1) {
        iters = 1;
    }
    uint64_t cycles;

    if (lua_isfunction(L, 1)) {
        cycles = time_lua_callable(L, 1, arg, (uint64_t)iters);
    } else {
        const char* name = luaL_checkstring(L, 1);
        size_t size = 0;
        void* owned = NULL;
        const void* data = artifact_load(name, &size, &owned);
        if (data == NULL) {
            return luaL_error(L, "no such artifact: %s", name);
        }
        if (is_elf(data, size)) {
            cycles = lab_bench(data, size, arg, (uint64_t)iters);
            if (owned != NULL) {
                heap_free(heap_default(), owned);
            }
        } else {
            int status = luaL_loadbuffer(L, data, size, name);
            if (owned != NULL) {
                heap_free(heap_default(), owned);
            }
            if (status != LUA_OK) {
                return lua_error(L);
            }
            cycles = time_lua_callable(L, lua_gettop(L), arg, (uint64_t)iters);
            lua_pop(L, 1); // the loaded chunk
        }
    }

    lua_pushinteger(L, (lua_Integer)cycles);
    lua_pushnumber(L, (lua_Number)cycles / (lua_Number)iters);
    return 2;
}

// Install the run/bench globals (called from luashell_init after the libraries
// are open, so bench of a script can use them).
void lua_run_open(lua_State* L)
{
    lua_pushcfunction(L, l_run);
    lua_setglobal(L, "run");
    lua_pushcfunction(L, l_bench);
    lua_setglobal(L, "bench");
}
