// Parallel Lua (M9): a lua_State per core plus the `thread` and `mem` libraries.
//
// Each core gets its own persistent worker interpreter with its OWN heap, so
// allocation is lock-free (a core only ever touches its own heap; the global
// heap stays BSP-only). Work is a function serialized to bytecode (lua_dump)
// and dispatched onto the SMP mailbox (smp_run_on); the worker loads and runs
// it in its state. Data crosses cores as explicit args (numbers/strings/bools
// and mem.shared handles) and through mem.shared buffers — never by sharing a
// lua_State (they aren't thread-safe). Because bytecode carries no upvalues,
// spawned functions must take their inputs as arguments.

#include <parallel.h>
#include <smp.h>
#include <memory.h>
#include <ktime.h>

#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define WORKER_HEAP_SZ (8 * 1024 * 1024) // per-core Lua heap
#define MAX_ARGS 16
#define ERR_MAX 200
#define SHARED_MT "juampi.shared"

// --- per-core state ---------------------------------------------------------

typedef struct {
    lua_State* L;        // this core's worker interpreter
    heap_allocator heap; // its private heap (a value type)
} worker_t;

// A cross-language value marshaled between states (copied, never shared refs).
enum { M_NIL = 0, M_BOOL, M_INT, M_NUM, M_STR, M_SHARED };
typedef struct {
    int type;
    union {
        int b;
        lua_Integer i;
        lua_Number n;
        struct {
            const char* p;
            size_t len;
        } s;
        struct {
            void* ptr;
            size_t size;
        } sh;
    } u;
} mval;

// One unit of dispatched work (one slot per core).
typedef struct {
    lua_State* L; // the target core's state
    void* code;   // owned bytecode copy (heap_default), or a borrowed pointer
    size_t code_len;
    bool own_code; // free code on finish?
    int nargs;
    mval args[MAX_ARGS];
    void* strbufs[MAX_ARGS]; // owned copies of string args, freed on finish
    int nstrbufs;
    volatile int status; // LUA_OK or an error code
    mval result;         // worker's return value (strings point into L)
    char err[ERR_MAX];
    bool busy; // has an in-flight spawn (for thread.spawn/join)
} job_t;

static worker_t* workers;
static job_t* jobs;
static uint64_t nworkers;

static void* kmalloc(size_t n)
{
    return alloc(&heap_default()->base, (ptrdiff_t)n, 16, 1);
}
static void kfree(void* p)
{
    heap_free(heap_default(), p);
}

// Lua allocator over a private heap_allocator (the `ud`). No lock: only the
// owning core calls it.
static void* worker_alloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
    heap_allocator* h = ud;
    if (nsize == 0) {
        heap_free(h, ptr);
        return NULL;
    }
    void* np = alloc(&h->base, (ptrdiff_t)nsize, 16, 1);
    if (ptr != NULL) {
        memcpy(np, ptr, osize < nsize ? osize : nsize);
        heap_free(h, ptr);
    }
    return np;
}

// --- marshaling -------------------------------------------------------------

static int marshal_from(lua_State* L, int idx, mval* m)
{
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
    case LUA_TNONE:
        m->type = M_NIL;
        return 1;
    case LUA_TBOOLEAN:
        m->type = M_BOOL;
        m->u.b = lua_toboolean(L, idx);
        return 1;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) {
            m->type = M_INT;
            m->u.i = lua_tointeger(L, idx);
        } else {
            m->type = M_NUM;
            m->u.n = lua_tonumber(L, idx);
        }
        return 1;
    case LUA_TSTRING:
        m->type = M_STR;
        m->u.s.p = lua_tolstring(L, idx, &m->u.s.len);
        return 1;
    case LUA_TUSERDATA: {
        void* p = luaL_testudata(L, idx, SHARED_MT);
        if (p != NULL) {
            struct sb {
                unsigned char* ptr;
                size_t size;
                int borrowed;
            }* sb = p;
            m->type = M_SHARED;
            m->u.sh.ptr = sb->ptr;
            m->u.sh.size = sb->size;
            return 1;
        }
        return 0;
    }
    default:
        return 0; // functions, tables, threads: unsupported across cores
    }
}

static void push_shared_view(lua_State* L, void* ptr, size_t size);

static void marshal_push(lua_State* L, const mval* m)
{
    switch (m->type) {
    case M_BOOL:
        lua_pushboolean(L, m->u.b);
        break;
    case M_INT:
        lua_pushinteger(L, m->u.i);
        break;
    case M_NUM:
        lua_pushnumber(L, m->u.n);
        break;
    case M_STR:
        lua_pushlstring(L, m->u.s.p, m->u.s.len);
        break;
    case M_SHARED:
        push_shared_view(L, m->u.sh.ptr, m->u.sh.size);
        break;
    default:
        lua_pushnil(L);
        break;
    }
}

// --- worker execution -------------------------------------------------------

// Run a job's bytecode + args in its state, capturing the result or error. Runs
// either on an application processor (via smp_run_on) or inline on the BSP.
static void run_job(job_t* j)
{
    lua_State* L = j->L;
    lua_settop(L, 0);
    int st = luaL_loadbuffer(L, j->code, j->code_len, "=worker");
    if (st == LUA_OK) {
        for (int i = 0; i < j->nargs; i++) {
            marshal_push(L, &j->args[i]);
        }
        st = lua_pcall(L, j->nargs, 1, 0);
    }
    if (st == LUA_OK) {
        if (!marshal_from(L, -1, &j->result)) {
            j->result.type = M_NIL;
        }
    } else {
        const char* e = lua_tostring(L, -1);
        size_t k = 0;
        if (e != NULL) {
            for (; e[k] != '\0' && k < ERR_MAX - 1; k++) {
                j->err[k] = e[k];
            }
        }
        j->err[k] = '\0';
    }
    j->status = st;
    // Leave the result on the worker stack so a string result stays alive until
    // the caller reads it (the next dispatch clears the stack).
}

static void worker_entry(void* ctx)
{
    run_job((job_t*)ctx);
}

// Post a job to an application processor: copy the bytecode and any string args
// into owned buffers (so they don't depend on the caller's Lua GC), then
// dispatch. Numbers/bools/shared handles are value-copied already.
static void start_worker(uint32_t core, const void* code, size_t len,
                         const mval* args, int nargs)
{
    job_t* j = &jobs[core];
    j->L = workers[core].L;
    j->code = kmalloc(len);
    memcpy(j->code, code, len);
    j->code_len = len;
    j->own_code = true;
    j->nargs = nargs;
    j->nstrbufs = 0;
    for (int i = 0; i < nargs; i++) {
        j->args[i] = args[i];
        if (args[i].type == M_STR) {
            void* s = kmalloc(args[i].u.s.len);
            memcpy(s, args[i].u.s.p, args[i].u.s.len);
            j->args[i].u.s.p = s;
            j->strbufs[j->nstrbufs++] = s;
        }
    }
    j->status = -1;
    smp_run_on(core, worker_entry, j);
}

// Wait for a dispatched job and reclaim its owned buffers. The result's strings
// still point into the worker state (valid until its next dispatch).
static int finish_worker(uint32_t core, mval* out)
{
    smp_join(core);
    job_t* j = &jobs[core];
    if (j->own_code) {
        kfree(j->code);
        j->code = NULL;
    }
    for (int i = 0; i < j->nstrbufs; i++) {
        kfree(j->strbufs[i]);
    }
    j->nstrbufs = 0;
    *out = j->result;
    return j->status;
}

// --- thread library ---------------------------------------------------------

static void require_bsp(lua_State* L)
{
    if (smp_this_cpu()->index != smp_bsp_index()) {
        luaL_error(L, "thread dispatch is only allowed from the main core");
    }
}

static int dump_writer(lua_State* L, const void* p, size_t sz, void* ud)
{
    (void)L;
    luaL_addlstring((luaL_Buffer*)ud, (const char*)p, sz);
    return 0;
}

// Reject functions that close over locals — bytecode can't carry upvalues, so
// their values would silently vanish. _ENV (rebound on load) is allowed.
static void check_no_upvalues(lua_State* L, int idx)
{
    for (int i = 1;; i++) {
        const char* name = lua_getupvalue(L, idx, i);
        if (name == NULL) {
            break;
        }
        lua_pop(L, 1);
        if (strcmp(name, "_ENV") != 0) {
            luaL_error(L,
                       "thread: function captures upvalue '%s'; pass data as "
                       "arguments instead",
                       name);
        }
    }
}

// Dump the function at `idx` to a bytecode string pushed on top; returns it via
// lua_tolstring. lua_dump needs the function on top, so push a copy and leave
// it: don't pop it afterwards, because for a large function luaL_addlstring has
// already pushed the buffer box above it, and popping would corrupt the buffer
// (this is why only big functions were affected). The leftover copy sits below
// the result and is discarded when the calling C function returns.
static const char* dump_fn(lua_State* L, int idx, size_t* len)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    lua_pushvalue(L, idx);
    if (lua_dump(L, dump_writer, &b, 1) != 0) {
        luaL_error(L, "thread: cannot serialize function");
    }
    luaL_pushresult(&b);
    return lua_tolstring(L, -1, len);
}

// thread.spawn(core, fn, ...args): run fn(core, ...args) on an application
// processor. Returns nothing; pair with thread.join(core).
static int l_spawn(lua_State* L)
{
    require_bsp(L);
    lua_Integer core = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (core < 0 || (uint64_t)core >= nworkers ||
        (uint32_t)core == smp_bsp_index()) {
        return luaL_error(L, "thread.spawn: core %d is not an application "
                             "processor",
                          (int)core);
    }
    if (jobs[core].busy) {
        return luaL_error(L, "thread.spawn: core %d already has a job",
                          (int)core);
    }
    check_no_upvalues(L, 2);

    int last = lua_gettop(L); // user args are 3..last
    size_t len;
    const char* code = dump_fn(L, 2, &len); // pushes bytecode (now at last+1)

    mval a[MAX_ARGS];
    a[0].type = M_INT;
    a[0].u.i = core;
    int n = 1;
    for (int i = 3; i <= last && n < MAX_ARGS; i++) {
        if (!marshal_from(L, i, &a[n])) {
            return luaL_error(L, "thread.spawn: argument %d has an unsupported "
                                 "type",
                              i - 2);
        }
        n++;
    }
    start_worker((uint32_t)core, code, len, a, n);
    jobs[core].busy = true;
    return 0;
}

// thread.join(core) -> result. Wait for the core's job; return its value or
// re-raise its error.
static int l_join(lua_State* L)
{
    require_bsp(L);
    lua_Integer core = luaL_checkinteger(L, 1);
    if (core < 0 || (uint64_t)core >= nworkers || !jobs[core].busy) {
        return luaL_error(L, "thread.join: core %d has no job", (int)core);
    }
    mval res;
    int st = finish_worker((uint32_t)core, &res);
    jobs[core].busy = false;
    if (st != LUA_OK) {
        return luaL_error(L, "thread worker on core %d: %s", (int)core,
                          jobs[core].err);
    }
    marshal_push(L, &res);
    return 1;
}

// thread.parallel(fn, ...args) -> results. Run fn(cpu, ...args) on every core
// (each AP dispatched, the BSP inline), join all, and return a table indexed by
// core+1 with each core's result. Raises if any worker errored.
static int l_parallel(lua_State* L)
{
    require_bsp(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    check_no_upvalues(L, 1);

    int last = lua_gettop(L); // user args 2..last
    size_t len;
    const char* code = dump_fn(L, 1, &len);

    mval uargs[MAX_ARGS];
    int nu = 0;
    for (int i = 2; i <= last && nu < MAX_ARGS - 1; i++) {
        if (!marshal_from(L, i, &uargs[nu])) {
            return luaL_error(L, "thread.parallel: argument %d has an "
                                 "unsupported type",
                              i - 1);
        }
        nu++;
    }

    uint32_t bsp = smp_bsp_index();

    // Dispatch each application processor with args = (cpu, uargs...).
    for (uint32_t c = 0; c < nworkers; c++) {
        if (c == bsp) {
            continue;
        }
        mval a[MAX_ARGS];
        a[0].type = M_INT;
        a[0].u.i = c;
        for (int k = 0; k < nu; k++) {
            a[k + 1] = uargs[k];
        }
        start_worker(c, code, len, a, nu + 1);
    }

    // Run the BSP's own share inline (its args/bytecode are live on this stack,
    // so no owned copies are needed).
    {
        job_t* jb = &jobs[bsp];
        jb->L = workers[bsp].L;
        jb->code = (void*)(uintptr_t)code;
        jb->code_len = len;
        jb->own_code = false;
        jb->nstrbufs = 0;
        jb->args[0].type = M_INT;
        jb->args[0].u.i = bsp;
        for (int k = 0; k < nu; k++) {
            jb->args[k + 1] = uargs[k];
        }
        jb->nargs = nu + 1;
        jb->status = -1;
        run_job(jb);
    }

    // Join, collect results (core+1), and note the first error if any.
    lua_newtable(L);
    int err_core = -1;
    for (uint32_t c = 0; c < nworkers; c++) {
        mval res;
        int st;
        if (c == bsp) {
            st = jobs[bsp].status;
            res = jobs[bsp].result;
        } else {
            st = finish_worker(c, &res);
        }
        if (st != LUA_OK) {
            if (err_core < 0) {
                err_core = (int)c;
            }
            lua_pushnil(L);
        } else {
            marshal_push(L, &res);
        }
        lua_rawseti(L, -2, (lua_Integer)c + 1);
    }
    if (err_core >= 0) {
        return luaL_error(L, "thread worker on core %d: %s", err_core,
                          jobs[err_core].err);
    }
    return 1;
}

static int l_cores(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)smp_cpu_count());
    return 1;
}
static int l_cpu(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)smp_this_cpu()->index);
    return 1;
}
static int l_trdtsc(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)rdtsc());
    return 1;
}
static int l_tns(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)ktime_ns());
    return 1;
}

static const luaL_Reg threadlib[] = {
        {"cores", l_cores}, {"cpu", l_cpu},         {"rdtsc", l_trdtsc},
        {"ns", l_tns},      {"spawn", l_spawn},     {"join", l_join},
        {"parallel", l_parallel}, {NULL, NULL},
};

int luaopen_thread(lua_State* L)
{
    luaL_newlib(L, threadlib);
    return 1;
}

// --- mem library (shared buffers) -------------------------------------------

typedef struct {
    unsigned char* ptr;
    size_t size;
    int borrowed; // a view of a buffer owned elsewhere: do not free
} shared_buf;

static void push_shared_view(lua_State* L, void* ptr, size_t size)
{
    shared_buf* sb = lua_newuserdatauv(L, sizeof(shared_buf), 0);
    sb->ptr = ptr;
    sb->size = size;
    sb->borrowed = 1;
    luaL_setmetatable(L, SHARED_MT);
}

// Wrap arbitrary kernel memory as a borrowed shared buffer (used by fb.canvas
// to expose the framebuffer). The metatable is registered by luaopen_mem.
void mem_push_view(lua_State* L, void* ptr, size_t size)
{
    push_shared_view(L, ptr, size);
}

// mem.shared(n) -> a zeroed n-byte buffer every core can read/write.
static int l_shared(lua_State* L)
{
    lua_Integer n = luaL_checkinteger(L, 1);
    if (n <= 0) {
        return luaL_error(L, "mem.shared: size must be positive");
    }
    unsigned char* p = kmalloc((size_t)n); // zeroed by the heap
    shared_buf* sb = lua_newuserdatauv(L, sizeof(shared_buf), 0);
    sb->ptr = p;
    sb->size = (size_t)n;
    sb->borrowed = 0;
    luaL_setmetatable(L, SHARED_MT);
    return 1;
}

static int l_shared_gc(lua_State* L)
{
    shared_buf* sb = luaL_checkudata(L, 1, SHARED_MT);
    if (!sb->borrowed && sb->ptr != NULL) {
        kfree(sb->ptr);
        sb->ptr = NULL;
    }
    return 0;
}

static int l_size(lua_State* L)
{
    shared_buf* sb = luaL_checkudata(L, 1, SHARED_MT);
    lua_pushinteger(L, (lua_Integer)sb->size);
    return 1;
}

// Bounds-check a [off, off+width) access into a shared buffer.
static unsigned char* at(lua_State* L, shared_buf* sb, lua_Integer off,
                         size_t width)
{
    if (off < 0 || (size_t)off + width > sb->size) {
        luaL_error(L, "mem: access at %d (width %d) out of bounds (size %d)",
                   (int)off, (int)width, (int)sb->size);
    }
    return sb->ptr + off;
}

#define ACCESSOR_INT(NAME, CTYPE, WIDTH)                                       \
    static int NAME(lua_State* L)                                              \
    {                                                                          \
        shared_buf* sb = luaL_checkudata(L, 1, SHARED_MT);                     \
        lua_Integer off = luaL_checkinteger(L, 2);                             \
        unsigned char* a = at(L, sb, off, WIDTH);                             \
        if (lua_isnoneornil(L, 3)) {                                          \
            CTYPE v;                                                           \
            memcpy(&v, a, WIDTH);                                              \
            lua_pushinteger(L, (lua_Integer)v);                               \
            return 1;                                                          \
        }                                                                      \
        CTYPE v = (CTYPE)luaL_checkinteger(L, 3);                             \
        memcpy(a, &v, WIDTH);                                                 \
        return 0;                                                             \
    }

ACCESSOR_INT(l_u8, uint8_t, 1)
ACCESSOR_INT(l_u16, uint16_t, 2)
ACCESSOR_INT(l_u32, uint32_t, 4)
ACCESSOR_INT(l_u64, uint64_t, 8)

static int l_f64(lua_State* L)
{
    shared_buf* sb = luaL_checkudata(L, 1, SHARED_MT);
    lua_Integer off = luaL_checkinteger(L, 2);
    unsigned char* a = at(L, sb, off, 8);
    if (lua_isnoneornil(L, 3)) {
        double v;
        memcpy(&v, a, 8);
        lua_pushnumber(L, (lua_Number)v);
        return 1;
    }
    double v = (double)luaL_checknumber(L, 3);
    memcpy(a, &v, 8);
    return 0;
}

static const luaL_Reg shared_methods[] = {
        {"size", l_size}, {"u8", l_u8},   {"u16", l_u16}, {"u32", l_u32},
        {"u64", l_u64},   {"f64", l_f64}, {NULL, NULL},
};

static const luaL_Reg memlib[] = {
        {"shared", l_shared},
        {NULL, NULL},
};

int luaopen_mem(lua_State* L)
{
    if (luaL_newmetatable(L, SHARED_MT)) {
        lua_pushcfunction(L, l_shared_gc);
        lua_setfield(L, -2, "__gc");
        lua_newtable(L);
        luaL_setfuncs(L, shared_methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
    luaL_newlib(L, memlib);
    return 1;
}

// --- engine bring-up --------------------------------------------------------

static void open_worker_libs(lua_State* L)
{
    static const luaL_Reg libs[] = {
            {LUA_GNAME, luaopen_base},         {LUA_TABLIBNAME, luaopen_table},
            {LUA_STRLIBNAME, luaopen_string},  {LUA_MATHLIBNAME, luaopen_math},
            {LUA_COLIBNAME, luaopen_coroutine}, {"thread", luaopen_thread},
            {"mem", luaopen_mem},
    };
    for (unsigned i = 0; i < sizeof(libs) / sizeof(libs[0]); i++) {
        luaL_requiref(L, libs[i].name, libs[i].func, 1);
        lua_pop(L, 1);
    }
}

void parallel_init(allocator* global)
{
    nworkers = smp_cpu_count();
    workers = new (global, worker_t, (ptrdiff_t)nworkers);
    jobs = new (global, job_t, (ptrdiff_t)nworkers);
    for (uint64_t i = 0; i < nworkers; i++) {
        void* region = alloc(global, WORKER_HEAP_SZ, 16, 1);
        workers[i].heap = heap_init(region, WORKER_HEAP_SZ);
        lua_State* Lw = lua_newstate(worker_alloc, &workers[i].heap);
        open_worker_libs(Lw);
        workers[i].L = Lw;
        jobs[i].busy = false;
    }
}

bool parallel_selftest(void)
{
    if (nworkers < 1) {
        return false;
    }
    uint32_t bsp = smp_bsp_index();
    lua_State* Lb = workers[bsp].L;
    lua_settop(Lb, 0);
    // A vararg chunk that returns its first argument (the core id).
    if (luaL_loadstring(Lb, "local a = ... ; return a") != LUA_OK) {
        return false;
    }
    luaL_Buffer b;
    luaL_buffinit(Lb, &b);
    lua_pushvalue(Lb, 1); // a copy on top for lua_dump; left in place (see
                          // dump_fn — popping it can corrupt the buffer box)
    if (lua_dump(Lb, dump_writer, &b, 1) != 0) {
        return false;
    }
    luaL_pushresult(&b);
    size_t len;
    const char* code = lua_tolstring(Lb, -1, &len);

    for (uint32_t c = 0; c < nworkers; c++) {
        if (c == bsp) {
            continue;
        }
        mval a = {.type = M_INT, .u.i = c};
        start_worker(c, code, len, &a, 1);
    }
    bool ok = true;
    for (uint32_t c = 0; c < nworkers; c++) {
        if (c == bsp) {
            continue;
        }
        mval r;
        int st = finish_worker(c, &r);
        if (st != LUA_OK || r.type != M_INT || r.u.i != (lua_Integer)c) {
            ok = false;
        }
    }
    lua_settop(Lb, 0);
    return ok;
}
