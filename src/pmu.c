// Intel architectural PMU driver (see pmu.h). Detection is CPUID leaf 0AH;
// programming is three MSR families: IA32_PERFEVTSELx selects a GP event,
// IA32_FIXED_CTR_CTRL enables the fixed counters, and IA32_PERF_GLOBAL_CTRL
// gates them all. Reads use rdpmc, which ring 0 may always execute. We never
// write the counters themselves: callers snapshot before/after and subtract,
// which sidesteps full-width-write quirks (48-bit counters wrap after hours).

#include <pmu.h>
#include <console.h>
#include <utils.h>

// MSRs (Intel SDM vol. 3, "Performance Monitoring").
#define MSR_PERFEVTSEL0 0x186
#define MSR_FIXED_CTR_CTRL 0x38D
#define MSR_PERF_GLOBAL_CTRL 0x38F

// IA32_PERFEVTSELx bits.
#define EVTSEL_USR (1u << 16)
#define EVTSEL_OS (1u << 17)
#define EVTSEL_EN (1u << 22)

static uint32_t version;  // architectural perfmon version (0 = no PMU)
static uint32_t gp_avail; // GP counters per core reported by CPUID
static int gp_used;       // GP slots we actually program

// The architectural GP events (CPUID-enumerated, portable, KVM-virtualized),
// in slot order. Fixed counters cover instructions/cycles/ref-cycles.
static const struct {
    uint8_t event, umask;
    const char* name;
} arch_events[PMU_GP] = {
        {0x2E, 0x4F, "llc_refs"},
        {0x2E, 0x41, "llc_misses"},
        {0xC4, 0x00, "branches"},
        {0xC5, 0x00, "branch_misses"},
};

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ __volatile__("wrmsr"
                         :
                         : "c"(msr), "a"((uint32_t)v),
                           "d"((uint32_t)(v >> 32)));
}

static inline uint64_t rdpmc(uint32_t sel)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdpmc" : "=a"(lo), "=d"(hi) : "c"(sel));
    return ((uint64_t)hi << 32) | lo;
}

bool pmu_present(void)
{
    // Fixed counters need architectural perfmon v2+; TCG reports version 0.
    return version >= 2;
}

uint32_t pmu_version(void)
{
    return version;
}

const char* pmu_gp_name(int i)
{
    return (i >= 0 && i < PMU_GP) ? arch_events[i].name : NULL;
}

int pmu_gp_in_use(void)
{
    return gp_used;
}

void pmu_start(void)
{
    if (!pmu_present()) {
        return;
    }
    for (int i = 0; i < gp_used; i++) {
        wrmsr(MSR_PERFEVTSEL0 + (uint32_t)i,
              (uint64_t)arch_events[i].event |
                      ((uint64_t)arch_events[i].umask << 8) | EVTSEL_USR |
                      EVTSEL_OS | EVTSEL_EN);
    }
    // Fixed counters 0-2: count in ring 0 and ring 3 (everything we run).
    wrmsr(MSR_FIXED_CTR_CTRL, 0x333);
    // Global enable: the GP slots in use + fixed 0-2.
    uint64_t mask = ((1ull << gp_used) - 1) | (0x7ull << 32);
    wrmsr(MSR_PERF_GLOBAL_CTRL, mask);
}

void pmu_stop(void)
{
    if (!pmu_present()) {
        return;
    }
    wrmsr(MSR_PERF_GLOBAL_CTRL, 0);
}

uint64_t pmu_read_fixed(int i)
{
    return rdpmc(0x40000000u | (uint32_t)i);
}

uint64_t pmu_read_gp(int i)
{
    return rdpmc((uint32_t)i);
}

// A loop with an exact instruction count (2 per iteration: dec + jnz), for the
// boot self-test's ground truth.
static void spin_instr(uint64_t iters)
{
    __asm__ __volatile__("1: dec %0; jnz 1b" : "+r"(iters));
}

void pmu_init(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(0x0A), "c"(0));
    version = eax & 0xFF;
    gp_avail = (eax >> 8) & 0xFF;
    uint32_t fixed = edx & 0x1F;
    if (version < 2 || fixed < PMU_FIXED || gp_avail == 0) {
        version = 0; // absent or too old to be useful (TCG lands here)
        console_printf("juampiOS: pmu absent (no architectural perfmon)\n");
        return;
    }
    gp_used = gp_avail < PMU_GP ? (int)gp_avail : PMU_GP;

    // Self-test: count a loop whose instruction total is known exactly. The
    // tolerance absorbs the handful of interrupts that land in the window.
    pmu_start();
    uint64_t i0 = pmu_read_fixed(0);
    spin_instr(1000000);
    uint64_t instr = pmu_read_fixed(0) - i0;
    pmu_stop();
    bool ok = instr >= 2000000 && instr < 2100000;
    console_printf("juampiOS: pmu v%u: %d gp + %u fixed, selftest instr=%llu "
                   "(expect ~2e6) %s\n",
                   version, gp_used, fixed, (unsigned long long)instr,
                   ok ? "OK" : "MISMATCH");
}
