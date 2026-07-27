// The `k` library: kernel introspection and control from Lua. Because the shell
// runs in ring 0, Lua has full access to the machine — this exposes it directly:
// the cycle counter and clock, memory and CPU inspection, raw memory and I/O
// ports, and symbolication. Poking a bad address or MSR faults into the
// (symbolized) exception handler; that is the deal for "full access".

#include <ktime.h>
#include <frames.h>
#include <ksym.h>
#include <ports.h>
#include <memory.h>
#include <smp.h>
#include <acpi.h>

#include <printf/printf.h>
#include <stdint.h>
#include <luadoc.h>

#include "lua.h"
#include "lauxlib.h"

// --- time / profiling -------------------------------------------------------

static int l_rdtsc(lua_State* L)
{
    lua_pushinteger(L, rdtsc());
    return 1;
}
static int l_ns(lua_State* L)
{
    lua_pushinteger(L, ktime_ns());
    return 1;
}
static int l_us(lua_State* L)
{
    lua_pushinteger(L, ktime_us());
    return 1;
}
static int l_ms(lua_State* L)
{
    lua_pushinteger(L, ktime_ms());
    return 1;
}
static int l_uptime(lua_State* L)
{
    lua_pushnumber(L, ktime_ns() / 1e9);
    return 1;
}
static int l_tsc_hz(lua_State* L)
{
    lua_pushinteger(L, tsc_hz());
    return 1;
}

// k.ncores() -> number of CPU cores online; k.cpu() -> index of the core this
// call runs on (the shell runs on the BSP, so 0, until Lua runs on APs in M9).
static int l_ncores(lua_State* L)
{
    lua_pushinteger(L, smp_cpu_count());
    return 1;
}
static int l_cpu(lua_State* L)
{
    lua_pushinteger(L, smp_this_cpu()->index);
    return 1;
}

// --- power / entropy --------------------------------------------------------

// k.shutdown() powers the machine off (ACPI S5); k.reboot() resets it. Neither
// returns.
static int l_shutdown(lua_State* L)
{
    (void)L;
    acpi_shutdown();
    return 0;
}
static int l_reboot(lua_State* L)
{
    (void)L;
    acpi_reboot();
    return 0;
}

// Whether the CPU has RDRAND, probed once and cached: CPUID is a serializing
// instruction (and a VM exit under virtualization), and the answer is fixed for
// the life of the machine.
static bool has_rdrand(void)
{
    static int cached = -1;
    if (cached < 0) {
        uint32_t a, b, c, d;
        __asm__ __volatile__("cpuid"
                             : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(1u), "c"(0u));
        cached = (c >> 30) & 1u; // CPUID.1:ECX.RDRAND
    }
    return cached;
}

// k.random() -> a random 64-bit integer, from the CPU's hardware RNG (RDRAND)
// when available, otherwise a TSC-seeded xorshift PRNG.
static int l_random(lua_State* L)
{
    uint64_t v = 0;
    if (has_rdrand()) {
        for (int i = 0; i < 16; i++) {
            unsigned char ok;
            __asm__ __volatile__("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
            if (ok) {
                lua_pushinteger(L, v);
                return 1;
            }
        }
    }
    static uint64_t s;
    if (s == 0) {
        s = rdtsc() | 1;
    }
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    lua_pushinteger(L, s);
    return 1;
}

// --- memory / cpu -----------------------------------------------------------

static int l_freeframes(lua_State* L)
{
    lua_pushinteger(L, frames_available());
    return 1;
}
static int l_freemem(lua_State* L)
{
    lua_pushinteger(L, (frames_available() * 4096ull));
    return 1;
}
static int l_totalmem(lua_State* L)
{
    lua_pushinteger(L, (frames_total() * 4096ull));
    return 1;
}

// k.cpuid(leaf [, subleaf]) -> eax, ebx, ecx, edx
static int l_cpuid(lua_State* L)
{
    uint32_t leaf = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t sub = (uint32_t)luaL_optinteger(L, 2, 0);
    uint32_t a, b, c, d;
    __asm__ __volatile__("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(leaf), "c"(sub));
    lua_pushinteger(L, a);
    lua_pushinteger(L, b);
    lua_pushinteger(L, c);
    lua_pushinteger(L, d);
    return 4;
}

// k.cpubrand() -> the CPU brand string (CPUID leaves 0x80000002-4).
static int l_cpubrand(lua_State* L)
{
    uint32_t r[12];
    for (int i = 0; i < 3; i++) {
        __asm__ __volatile__("cpuid"
                             : "=a"(r[i * 4]), "=b"(r[i * 4 + 1]),
                               "=c"(r[i * 4 + 2]), "=d"(r[i * 4 + 3])
                             : "a"(0x80000002u + i));
    }
    lua_pushstring(L, (const char*)r);
    return 1;
}

static int l_rdmsr(lua_State* L)
{
    uint32_t msr = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    lua_pushinteger(L, (((uint64_t)hi << 32) | lo));
    return 1;
}
static int l_wrmsr(lua_State* L)
{
    uint32_t msr = (uint32_t)luaL_checkinteger(L, 1);
    uint64_t val = (uint64_t)luaL_checkinteger(L, 2);
    __asm__ __volatile__("wrmsr" ::"c"(msr), "a"((uint32_t)val),
                         "d"((uint32_t)(val >> 32)));
    return 0;
}

// --- raw memory / ports -----------------------------------------------------

static int l_peek8(lua_State* L)
{
    lua_pushinteger(L, *(volatile uint8_t*)(uintptr_t)luaL_checkinteger(L, 1));
    return 1;
}
static int l_peek16(lua_State* L)
{
    lua_pushinteger(L, *(volatile uint16_t*)(uintptr_t)luaL_checkinteger(L, 1));
    return 1;
}
static int l_peek32(lua_State* L)
{
    lua_pushinteger(L, *(volatile uint32_t*)(uintptr_t)luaL_checkinteger(L, 1));
    return 1;
}
static int l_peek64(lua_State* L)
{
    lua_pushinteger(L, *(volatile uint64_t*)(uintptr_t)
                               luaL_checkinteger(L, 1));
    return 1;
}
static int l_poke8(lua_State* L)
{
    *(volatile uint8_t*)(uintptr_t)luaL_checkinteger(L, 1) =
            (uint8_t)luaL_checkinteger(L, 2);
    return 0;
}
static int l_poke16(lua_State* L)
{
    *(volatile uint16_t*)(uintptr_t)luaL_checkinteger(L, 1) =
            (uint16_t)luaL_checkinteger(L, 2);
    return 0;
}
static int l_poke32(lua_State* L)
{
    *(volatile uint32_t*)(uintptr_t)luaL_checkinteger(L, 1) =
            (uint32_t)luaL_checkinteger(L, 2);
    return 0;
}
static int l_poke64(lua_State* L)
{
    *(volatile uint64_t*)(uintptr_t)luaL_checkinteger(L, 1) =
            (uint64_t)luaL_checkinteger(L, 2);
    return 0;
}

static int l_inb(lua_State* L)
{
    lua_pushinteger(L, inb((uint16_t)luaL_checkinteger(L, 1)));
    return 1;
}
static int l_outb(lua_State* L)
{
    outb((uint16_t)luaL_checkinteger(L, 1), (uint8_t)luaL_checkinteger(L, 2));
    return 0;
}

// k.hexdump(addr [, len]) -> prints a canonical hex/ASCII dump.
static int l_hexdump(lua_State* L)
{
    uintptr_t a = (uintptr_t)luaL_checkinteger(L, 1);
    lua_Integer len = luaL_optinteger(L, 2, 64);
    const uint8_t* p = (const uint8_t*)a;
    for (lua_Integer i = 0; i < len; i += 16) {
        printf("%016lx  ", (unsigned long)(a + i));
        for (lua_Integer j = 0; j < 16; j++) {
            if (i + j < len) {
                printf("%02x ", p[i + j]);
            } else {
                printf("   ");
            }
        }
        printf(" |");
        for (lua_Integer j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = p[i + j];
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("|\n");
    }
    return 0;
}

// --- symbolication ----------------------------------------------------------

static int l_sym(lua_State* L)
{
    uint64_t addr = (uint64_t)luaL_checkinteger(L, 1);
    uint64_t off = 0;
    const char* name = ksym_lookup(addr, &off);
    if (name == NULL) {
        return 0;
    }
    lua_pushstring(L, name);
    lua_pushinteger(L, off);
    return 2;
}

static int l_backtrace(lua_State* L)
{
    (void)L;
    backtrace();
    return 0;
}

#define ADDR {"addr", "number", "address (may fault if invalid)"}
#define VAL(t) {"value", "number", t}

static const lua_fndoc klib[] = {
        {"rdtsc", l_rdtsc, "Read the CPU cycle counter (TSC).",
         .rets = {{"cycles", "number", "current TSC value"}}},
        {"ns", l_ns, "Monotonic time since boot, in nanoseconds.",
         .rets = {{"ns", "number", "nanoseconds"}}},
        {"us", l_us, "Monotonic time since boot, in microseconds.",
         .rets = {{"us", "number", "microseconds"}}},
        {"ms", l_ms, "Monotonic time since boot, in milliseconds.",
         .rets = {{"ms", "number", "milliseconds"}}},
        {"uptime", l_uptime, "Seconds since boot (fractional).",
         .rets = {{"seconds", "number", "uptime in seconds"}}},
        {"tsc_hz", l_tsc_hz, "Calibrated TSC frequency.",
         .rets = {{"hz", "number", "TSC ticks per second"}}},
        {"ncores", l_ncores, "Number of CPU cores online.",
         .rets = {{"n", "number", "core count"}}},
        {"cpu", l_cpu, "Index of the core running this call.",
         .rets = {{"index", "number", "0 for the BSP"}}},
        {"shutdown", l_shutdown, "Power the machine off (ACPI S5). No return."},
        {"reboot", l_reboot, "Reset the machine. No return."},
        {"random", l_random, "A random 64-bit integer (RDRAND, else xorshift).",
         .rets = {{"value", "number", "random 64-bit integer"}}},
        {"freeframes", l_freeframes, "Free physical page frames.",
         .rets = {{"frames", "number", "count of free 4 KiB frames"}}},
        {"freemem", l_freemem, "Free physical memory in bytes.",
         .rets = {{"bytes", "number", "free bytes"}}},
        {"totalmem", l_totalmem, "Total managed physical memory in bytes.",
         .rets = {{"bytes", "number", "total bytes"}}},
        {"cpuid", l_cpuid, "Execute CPUID.",
         .args = {{"leaf", "number", "CPUID leaf (EAX)"},
                  {"subleaf", "number?", "sub-leaf (ECX), default 0"}},
         .rets = {{"eax", "number", ""},
                  {"ebx", "number", ""},
                  {"ecx", "number", ""},
                  {"edx", "number", ""}}},
        {"cpubrand", l_cpubrand, "The CPU brand string.",
         .rets = {{"brand", "string", "e.g. the marketing name"}}},
        {"rdmsr", l_rdmsr, "Read a model-specific register.",
         .args = {{"msr", "number", "MSR index"}},
         .rets = {{"value", "number", "64-bit MSR value"}}},
        {"wrmsr", l_wrmsr, "Write a model-specific register.",
         .args = {{"msr", "number", "MSR index"},
                  {"value", "number", "64-bit value to write"}}},
        {"peek8", l_peek8, "Read a byte from memory.",
         .args = {ADDR}, .rets = {VAL("the byte")}},
        {"peek16", l_peek16, "Read a 16-bit word from memory.",
         .args = {ADDR}, .rets = {VAL("the word")}},
        {"peek32", l_peek32, "Read a 32-bit word from memory.",
         .args = {ADDR}, .rets = {VAL("the dword")}},
        {"peek64", l_peek64, "Read a 64-bit word from memory.",
         .args = {ADDR}, .rets = {VAL("the qword")}},
        {"poke8", l_poke8, "Write a byte to memory.",
         .args = {ADDR, VAL("byte to store")}},
        {"poke16", l_poke16, "Write a 16-bit word to memory.",
         .args = {ADDR, VAL("word to store")}},
        {"poke32", l_poke32, "Write a 32-bit word to memory.",
         .args = {ADDR, VAL("dword to store")}},
        {"poke64", l_poke64, "Write a 64-bit word to memory.",
         .args = {ADDR, VAL("qword to store")}},
        {"inb", l_inb, "Read a byte from an I/O port.",
         .args = {{"port", "number", "I/O port"}},
         .rets = {VAL("the byte read")}},
        {"outb", l_outb, "Write a byte to an I/O port.",
         .args = {{"port", "number", "I/O port"}, VAL("byte to write")}},
        {"hexdump", l_hexdump, "Print a canonical hex/ASCII dump.",
         .args = {ADDR, {"len", "number?", "bytes to dump, default 64"}}},
        {"sym", l_sym, "Resolve an address to a kernel symbol.",
         .args = {ADDR,
                  },
         .rets = {{"name", "string?", "symbol name, or nil if unknown"},
                  {"offset", "number?", "offset past the symbol"}}},
        {"backtrace", l_backtrace, "Print a symbolized stack backtrace."},
        {0},
};

#undef ADDR
#undef VAL

int luaopen_k(lua_State* L)
{
    luadoc_newlib(L, klib);
    return 1;
}
