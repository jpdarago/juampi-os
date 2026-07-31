// The `perf` library: hardware performance counters for the shell (see
// include/pmu.h and docs/perf-lab.md). Measurements read the per-core PMU —
// instructions, cycles, reference cycles, and the architectural LLC/branch
// events — so an algorithm can be judged by IPC and cache behavior, not just
// wall cycles. Absent hardware (TCG) degrades to perf.available() == false.

#include <pmu.h>
#include <ktime.h>
#include <luadoc.h>

#include "lua.h"
#include "lauxlib.h"

struct snap {
    uint64_t fixed[PMU_FIXED];
    uint64_t gp[PMU_GP];
    uint64_t ns;
};

static void snap_read(struct snap* s)
{
    s->ns = ktime_ns();
    for (int i = 0; i < PMU_FIXED; i++) {
        s->fixed[i] = pmu_read_fixed(i);
    }
    for (int i = 0; i < pmu_gp_in_use(); i++) {
        s->gp[i] = pmu_read_gp(i);
    }
}

// Counts for one measured window, overhead-corrected by the caller.
struct counts {
    uint64_t fixed[PMU_FIXED];
    uint64_t gp[PMU_GP];
    uint64_t ns;
};

static void snap_delta(const struct snap* a, const struct snap* b,
                       struct counts* d)
{
    for (int i = 0; i < PMU_FIXED; i++) {
        d->fixed[i] = b->fixed[i] - a->fixed[i];
    }
    for (int i = 0; i < pmu_gp_in_use(); i++) {
        d->gp[i] = b->gp[i] - a->gp[i];
    }
    d->ns = b->ns - a->ns;
}

// The cost of an empty measurement bracket itself (snap + snap), measured once
// and subtracted from every perf.measure() so tiny workloads aren't inflated.
static struct counts overhead;
static bool calibrated;

static void calibrate(void)
{
    struct snap a, b;
    struct counts best = {0};
    for (int t = 0; t < 8; t++) {
        snap_read(&a);
        snap_read(&b);
        struct counts d;
        snap_delta(&a, &b, &d);
        if (t == 0 || d.fixed[0] < best.fixed[0]) {
            best = d;
        }
    }
    best.ns = 0; // keep wall time uncorrected; it is not a counter
    overhead = best;
    calibrated = true;
}

static uint64_t sub_floor(uint64_t v, uint64_t o)
{
    return v > o ? v - o : 0;
}

// Push the results table for a window: named fixed + GP counts, ipc, time_ns.
static void push_counts(lua_State* L, const struct counts* d, lua_Integer n)
{
    static const char* fixed_names[PMU_FIXED] = {"instructions", "cycles",
                                                 "ref_cycles"};
    lua_createtable(L, 0, PMU_FIXED + PMU_GP + 3);
    for (int i = 0; i < PMU_FIXED; i++) {
        lua_pushinteger(L, (lua_Integer)d->fixed[i]);
        lua_setfield(L, -2, fixed_names[i]);
    }
    for (int i = 0; i < pmu_gp_in_use(); i++) {
        lua_pushinteger(L, (lua_Integer)d->gp[i]);
        lua_setfield(L, -2, pmu_gp_name(i));
    }
    if (d->fixed[1] > 0) {
        lua_pushnumber(L, (lua_Number)d->fixed[0] / (lua_Number)d->fixed[1]);
        lua_setfield(L, -2, "ipc");
    }
    lua_pushinteger(L, (lua_Integer)d->ns);
    lua_setfield(L, -2, "time_ns");
    lua_pushinteger(L, n);
    lua_setfield(L, -2, "n");
}

static int l_available(lua_State* L)
{
    lua_pushboolean(L, pmu_present());
    return 1;
}

static int l_events(lua_State* L)
{
    lua_createtable(L, PMU_FIXED + PMU_GP, 0);
    const char* fixed_names[PMU_FIXED] = {"instructions", "cycles",
                                          "ref_cycles"};
    int k = 1;
    for (int i = 0; i < PMU_FIXED; i++) {
        lua_pushstring(L, fixed_names[i]);
        lua_rawseti(L, -2, k++);
    }
    for (int i = 0; i < pmu_gp_in_use(); i++) {
        lua_pushstring(L, pmu_gp_name(i));
        lua_rawseti(L, -2, k++);
    }
    return 1;
}

static struct snap open_snap;
static bool open_active;

static int l_start(lua_State* L)
{
    if (!pmu_present()) {
        lua_pushnil(L);
        lua_pushstring(L, "no PMU (TCG or pre-v2 hardware)");
        return 2;
    }
    pmu_start();
    open_active = true;
    snap_read(&open_snap);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_stop(lua_State* L)
{
    if (!open_active) {
        lua_pushnil(L);
        lua_pushstring(L, "perf.start() was not called");
        return 2;
    }
    struct snap b;
    snap_read(&b);
    pmu_stop();
    open_active = false;
    struct counts d;
    snap_delta(&open_snap, &b, &d);
    push_counts(L, &d, 1);
    return 1;
}

// perf.measure(fn [,arg [,iters=1]]) -> counts table for the whole run.
static int l_measure(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int has_arg = !lua_isnoneornil(L, 2);
    lua_Integer iters = luaL_optinteger(L, 3, 1);
    if (iters < 1) {
        iters = 1;
    }
    if (!pmu_present()) {
        lua_pushnil(L);
        lua_pushstring(L, "no PMU (TCG or pre-v2 hardware)");
        return 2;
    }
    pmu_start();
    if (!calibrated) {
        calibrate();
    }
    struct snap a, b;
    snap_read(&a);
    for (lua_Integer i = 0; i < iters; i++) {
        lua_pushvalue(L, 1);
        if (has_arg) {
            lua_pushvalue(L, 2);
        }
        lua_call(L, has_arg ? 1 : 0, 0);
    }
    snap_read(&b);
    pmu_stop();
    struct counts d;
    snap_delta(&a, &b, &d);
    for (int i = 0; i < PMU_FIXED; i++) {
        d.fixed[i] = sub_floor(d.fixed[i], overhead.fixed[i]);
    }
    for (int i = 0; i < pmu_gp_in_use(); i++) {
        d.gp[i] = sub_floor(d.gp[i], overhead.gp[i]);
    }
    push_counts(L, &d, iters);
    return 1;
}

static const struct lua_fndoc perflib[] = {
        {"available", l_available,
         "Whether hardware counters exist (false under TCG).",
         .rets = {{"ok", "boolean", "true when the PMU is usable"}}},
        {"events", l_events, "The event names a measurement reports.",
         .rets = {{"names", "table", "array of counter names"}}},
        {"measure", l_measure,
         "Run fn and return its hardware counts (overhead-corrected).",
         .args = {{"fn", "function", "the workload to measure"},
                  {"arg", "any?", "optional argument passed to fn"},
                  {"iters", "number?", "repetitions inside the window (1)"}},
         .rets = {{"counts", "table?",
                   "instructions/cycles/ipc/llc_*/branch_*/time_ns/n"},
                  {"err", "string?", "message when counts is nil"}}},
        {"start", l_start, "Begin counting until perf.stop().",
         .rets = {{"ok", "boolean?", "true, or nil without a PMU"},
                  {"err", "string?", "message when ok is nil"}}},
        {"stop", l_stop, "Stop counting and return the counts table.",
         .rets = {{"counts", "table?", "see perf.measure"},
                  {"err", "string?", "message when counts is nil"}}},
        {0},
};

int luaopen_perf(lua_State* L)
{
    luadoc_newlib(L, perflib);
    return 1;
}
