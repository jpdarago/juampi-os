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

#include <printf/printf.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

// --- time / profiling -------------------------------------------------------

static int l_rdtsc(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)rdtsc());
    return 1;
}
static int l_ns(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)ktime_ns());
    return 1;
}
static int l_us(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)ktime_us());
    return 1;
}
static int l_ms(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)ktime_ms());
    return 1;
}
static int l_uptime(lua_State* L)
{
    lua_pushnumber(L, (lua_Number)ktime_ns() / 1e9);
    return 1;
}
static int l_tsc_hz(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)tsc_hz());
    return 1;
}

// k.ncores() -> number of CPU cores online; k.cpu() -> index of the core this
// call runs on (the shell runs on the BSP, so 0, until Lua runs on APs in M9).
static int l_ncores(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)smp_cpu_count());
    return 1;
}
static int l_cpu(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)smp_this_cpu()->index);
    return 1;
}

// --- memory / cpu -----------------------------------------------------------

static int l_freeframes(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)frames_available());
    return 1;
}
static int l_freemem(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)(frames_available() * 4096ull));
    return 1;
}
static int l_totalmem(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)(frames_total() * 4096ull));
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
    lua_pushinteger(L, (lua_Integer)(((uint64_t)hi << 32) | lo));
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
    lua_pushinteger(L, (lua_Integer)*(volatile uint64_t*)(uintptr_t)
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
    lua_pushinteger(L, (lua_Integer)off);
    return 2;
}

static int l_backtrace(lua_State* L)
{
    (void)L;
    backtrace();
    return 0;
}

static const luaL_Reg klib[] = {
        {"rdtsc", l_rdtsc},       {"ns", l_ns},
        {"us", l_us},             {"ms", l_ms},
        {"uptime", l_uptime},     {"tsc_hz", l_tsc_hz},
        {"ncores", l_ncores},     {"cpu", l_cpu},
        {"freeframes", l_freeframes},
        {"freemem", l_freemem},   {"totalmem", l_totalmem},
        {"cpuid", l_cpuid},       {"cpubrand", l_cpubrand},
        {"rdmsr", l_rdmsr},       {"wrmsr", l_wrmsr},
        {"peek8", l_peek8},       {"peek16", l_peek16},
        {"peek32", l_peek32},     {"peek64", l_peek64},
        {"poke8", l_poke8},       {"poke16", l_poke16},
        {"poke32", l_poke32},     {"poke64", l_poke64},
        {"inb", l_inb},           {"outb", l_outb},
        {"hexdump", l_hexdump},   {"sym", l_sym},
        {"backtrace", l_backtrace}, {NULL, NULL},
};

int luaopen_k(lua_State* L)
{
    luaL_newlib(L, klib);
    return 1;
}
