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
#include <elf64.h>   // elf64_symbol — tell a hosted program from a lab one
#include <syscall.h> // hosted_run
#include <memory.h>
#include <ktime.h>
#include <console.h>
#include <str.h>
#include <utils.h> // memcpy
#include <ui.h>    // canvas window for native lab programs on the desktop
#include <gfx.h>   // off-screen render target

#include <printf/printf.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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
        lua_pushinteger(L, arg);
        lua_call(L, 1, 0);
    }
    return rdtsc() - start;
}

// --- run() with no argument: list the runnable artifacts -------------------
// The same sources run() resolves from: Limine modules + the ext2 search dirs
// (/, /scripts, /lab). Deduped, boot-only files hidden, sorted.

#define RUN_MAX 128
#define RUN_NAMELEN 64
struct runlist {
    char names[RUN_MAX][RUN_NAMELEN];
    int n;
};

// The last path segment (after the final '/'), or the whole view if none.
static struct str path_base(struct str p)
{
    for (size_t i = p.len; i > 0; i--) {
        if (p.data[i - 1] == '/') {
            return str_span(p.data + i, p.len - i);
        }
    }
    return p;
}

// Record a runnable name: a .lua/.elf basename, minus boot infra and duplicates.
static void runlist_add(struct runlist* rl, struct str name)
{
    if (!str_has_suffix(name, S(".lua")) && !str_has_suffix(name, S(".elf"))) {
        return;
    }
    if (str_eq(name, S("prelude.lua")) || str_eq(name, S("init.lua"))) {
        return; // auto-loaded at boot, not something you run by hand
    }
    for (int i = 0; i < rl->n; i++) {
        if (str_eq(str_from(rl->names[i]), name)) {
            return; // e.g. a module also present on the disk
        }
    }
    if (rl->n < RUN_MAX) {
        str_copy(rl->names[rl->n], RUN_NAMELEN, name);
        rl->n++;
    }
}

static void runlist_emit(void* ctx, const char* name, uint32_t inode,
                         uint8_t type)
{
    (void)inode;
    (void)type;
    runlist_add((struct runlist*)ctx, str_from(name));
}

static bool name_less(const char* a, const char* b)
{
    for (size_t i = 0;; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca != cb) {
            return ca < cb;
        }
        if (ca == 0) {
            return false;
        }
    }
}

static int l_run_list(void)
{
    struct runlist rl = {.n = 0};
    for (size_t i = 0; i < kmodule_count(); i++) {
        runlist_add(&rl, path_base(str_from(kmodule_path(i))));
    }
    ext2_list("/", runlist_emit, &rl);
    ext2_list("/scripts", runlist_emit, &rl);
    ext2_list("/lab", runlist_emit, &rl);

    for (int i = 1; i < rl.n; i++) { // insertion sort (small n)
        char key[RUN_NAMELEN];
        memcpy(key, rl.names[i], RUN_NAMELEN);
        int j = i - 1;
        while (j >= 0 && name_less(key, rl.names[j])) {
            memcpy(rl.names[j + 1], rl.names[j], RUN_NAMELEN);
            j--;
        }
        memcpy(rl.names[j + 1], key, RUN_NAMELEN);
    }

    console_print("runnable with run(\"name\"):\n");
    for (int i = 0; i < rl.n; i++) {
        console_print("  ");
        console_print(rl.names[i]);
        console_print("\n");
    }
    return 0;
}

// run(name [,arg]): dispatch by artifact type. An ELF is loaded and called in
// ring 0 (returning its result); anything else is loaded and executed as a Lua
// chunk (receiving `arg` as a vararg, returning its own values as before).
// With no argument, list what can be run.
static int l_run(lua_State* L)
{
    if (lua_isnoneornil(L, 1)) {
        return l_run_list();
    }
    const char* name = luaL_checkstring(L, 1);
    size_t size = 0;
    void* owned = NULL;
    const void* data = artifact_load(name, &size, &owned);
    if (data == NULL) {
        return luaL_error(L, "no such artifact: %s", name);
    }

    if (is_elf(data, size)) {
        // A hosted (newlib) program defines _start (via our crt0); it does text
        // I/O through syscalls, so just run it — its stdout already flows to the
        // console/terminal. Lab programs (entry `bench`) take the path below.
        if (elf64_symbol(data, "_start") != 0) {
            char arg0[64];
            size_t k = 0;
            while (name[k] != '\0' && k < sizeof(arg0) - 1) {
                arg0[k] = name[k];
                k++;
            }
            arg0[k] = '\0';
            char* av[2] = {arg0, NULL};
            int r = hosted_run(data, size, 1, av);
            if (owned != NULL) {
                ext2_free(owned);
            }
            lua_pushinteger(L, r);
            return 1;
        }
        long arg = (long)luaL_optinteger(L, 2, 0);
        // On the windowed desktop, render the program into an off-screen canvas
        // buffer; if it fetched the framebuffer (drew graphics), show it in a
        // window, else it was a text program whose output already went to the
        // terminal. The compositor would otherwise paint over raw VRAM.
        if (ui_available()) {
            int cw = 640, ch = 400;
            uint32_t* buf = new (&heap_default()->base, uint32_t,
                                 (ptrdiff_t)cw * ch);
            long r = lab_run(data, size, arg, buf, (uint64_t)cw, (uint64_t)ch);
            if (lab_drew()) {
                ui_open_canvas(name, buf, cw, ch); // takes ownership of buf
            } else {
                heap_free(heap_default(), buf);
            }
            if (owned != NULL) {
                ext2_free(owned);
            }
            lua_pushinteger(L, r);
            return 1;
        }
        long r = lab_run(data, size, arg, NULL, 0, 0);
        if (owned != NULL) {
            ext2_free(owned);
        }
        lua_pushinteger(L, r);
        return 1;
    }

    int base = lua_gettop(L);
    int status = luaL_loadbuffer(L, data, size, name);
    if (owned != NULL) {
        ext2_free(owned); // loadbuffer copied the bytes
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
                ext2_free(owned);
            }
        } else {
            int status = luaL_loadbuffer(L, data, size, name);
            if (owned != NULL) {
                ext2_free(owned);
            }
            if (status != LUA_OK) {
                return lua_error(L);
            }
            cycles = time_lua_callable(L, lua_gettop(L), arg, (uint64_t)iters);
            lua_pop(L, 1); // the loaded chunk
        }
    }

    lua_pushinteger(L, cycles);
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
