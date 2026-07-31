#ifndef __PMU_H
#define __PMU_H

#include <stdint.h>
#include <stdbool.h>

// Intel performance-monitoring unit (architectural perfmon, CPUID leaf 0AH):
// per-core hardware counters for noise-free measurement — the fixed counters
// (instructions retired, unhalted core cycles, unhalted reference cycles) plus
// general-purpose counters programmed with the architectural events, which are
// enumerated by CPUID, identical on every Intel core since 2006, and
// virtualized by KVM's vPMU (dev QEMU runs `-cpu host`; the same silicon is
// the XPS bare-metal target). Absent under TCG — callers gate on
// pmu_present(). Counters are per-core; this v1 programs and reads on the
// calling core only (the BSP for the Lua shell).

#define PMU_FIXED 3 // fixed counters we use: 0=instructions 1=cycles 2=ref
#define PMU_GP 4    // architectural GP events we program (see pmu_gp_name)

void pmu_init(void); // detect via CPUID; safe and quiet when absent
bool pmu_present(void);
uint32_t pmu_version(void);

// Names for the PMU_GP general-purpose event slots, in programming order:
// 0=llc_refs 1=llc_misses 2=branches 3=branch_misses.
const char* pmu_gp_name(int i);
// GP slots actually programmed (min(PMU_GP, hardware counters)).
int pmu_gp_in_use(void);

// Program the fixed counters + the architectural GP events on this core and
// enable them globally. Counters then free-run; take before/after snapshots
// with the readers below and subtract. pmu_stop() disables them again.
void pmu_start(void);
void pmu_stop(void);

// Raw counter reads (rdpmc — ~30 cycles). Valid between start and stop.
uint64_t pmu_read_fixed(int i); // 0=instructions 1=core cycles 2=ref cycles
uint64_t pmu_read_gp(int i);    // 0..pmu_gp_in_use()-1

#endif
