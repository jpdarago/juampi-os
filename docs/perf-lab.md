---
title: Performance lab (PMU counters)
tags: [design, performance, pmu, msr, planned]
status: planned
related: ["[[api-layers]]", "[[doom-port]]", "[[Index]]"]
created: 2026-07-31
---

# Performance lab: hardware counters for noise-free measurement

> [!abstract] Goal
> Expose the Intel performance-monitoring unit (PMU) as a first-class
> measurement tool: a `perf.*` Lua module and an upgraded `bench()` that
> report instructions, IPC, cache misses, and branch behavior — not just
> TSC cycles. The platform's unfair advantage is that ring 0 with no
> scheduler and no unrequested interrupts can measure architectural truth
> that a hosted OS cannot; this makes that advantage usable from the shell.

## Background

The kernel already exposes `k.rdtsc`, `k.rdmsr/wrmsr`, and `k.cpuid`, and
`bench()` measures cycles per call via the TSC. Cycles alone cannot answer
the questions performance experiments actually ask: how many instructions
retired (IPC), whether the working set fits in cache, and whether branches
predict. The PMU answers all of these with per-core hardware counters.

Target hardware: Tiger Lake H (the dev machine's i7-11800H and the XPS 15
9510 share it) — architectural PMU **version 5**: 4 fixed counters
(instructions, core cycles, reference cycles, topdown slots) and 8
general-purpose counters per core (4 with hyper-threading), 48-bit width.
Under QEMU this requires KVM with `-cpu host` (the vPMU virtualizes
counting mode); under TCG the PMU is absent and the module must degrade
gracefully.

## Design

### Kernel driver (`src/pmu.c`, `include/pmu.h`)

- `pmu_init()`: read CPUID leaf 0AH — PMU version, counter counts and
  widths, and the architectural-event availability mask. Store per-boot;
  `pmu_present()` reports availability (false under TCG).
- `pmu_program(slot, event, umask, flags)`: write `IA32_PERFEVTSELx`
  (0x186+i) with USR=0, OS=1, EN=1. Fixed counters enable via
  `IA32_FIXED_CTR_CTRL` (0x38D); global enable via
  `IA32_PERF_GLOBAL_CTRL` (0x38F).
- `pmu_read(slot)`: `rdpmc` (bit 30 selects fixed counters) — ~30 cycles,
  no MSR round-trip. Ring 0 may always execute `rdpmc`.
- Counters are per-core. Version 1 measures on the calling core (the BSP
  for Lua). A later milestone wires per-worker measurement through
  `thread.parallel` / the lab API.

### Event set (version 1)

Only the seven **architectural events** — enumerated by CPUID, identical
encodings on every Intel core since 2006, and virtualized by KVM — so
every measurement is portable between dev QEMU and the XPS:

| Name | Source | Encoding |
|------|--------|----------|
| `instructions` | fixed 0 | — |
| `cycles` | fixed 1 | — |
| `ref_cycles` | fixed 2 | — |
| `llc_refs` | GP | event 0x2E, umask 0x4F |
| `llc_misses` | GP | event 0x2E, umask 0x41 |
| `branches` | GP | event 0xC4, umask 0x00 |
| `branch_misses` | GP | event 0xC5, umask 0x00 |

Model-specific events (L1/L2 detail, stalls, topdown) are a follow-up;
the module's event table is data, so adding them is additive.

### Lua surface (`src/lua/lua_perf.c`, luadoc-registered)

- `perf.available()` → boolean (false under TCG; callers degrade).
- `perf.events()` → the supported event list.
- `perf.measure(fn|name [, arg [, iters]])` → table:
  `{cycles=…, instructions=…, ipc=…, llc_misses=…, …}` per call,
  overhead-calibrated (an empty measured region is subtracted).
- `perf.start{events…}` / `perf.stop()` → bracket arbitrary shell work
  interactively.
- `bench()` grows two result columns when the PMU is present:
  instructions per call and IPC. Existing callers see no change.

### Measurement discipline

- Overhead calibration at init: measure the empty region, store, subtract.
- Optional `irqs_off = true` for short regions (cli/sti bracket): removes
  the 100 Hz tick and audio/NIC interrupts from the window. Bounded use
  only; documented as such.
- Turbo makes `cycles ≠ ref_cycles`; the ratio itself is reported (it is
  the effective frequency multiplier). Frequency pinning via
  `IA32_PERF_CTL` is a follow-up under a future power/thermal note.

## Milestones

1. **QEMU realism**: add `-cpu host` (KVM) to `make run` and the smoke
   harness, keeping the TCG fallback path tested (CI uses TCG — the PMU
   self-test must skip cleanly there).
2. **Fixed counters**: `pmu.c` detect + fixed-counter bring-up, plus a
   boot self-test — count a loop with a known instruction total and assert
   the counter lands within tolerance (like the existing SMP/FP boot
   checks).
3. **GP events + `perf.*`**: the architectural event table, programming,
   `perf.measure`/`start`/`stop`, luadoc entries.
4. **`bench()` integration** and a `tests/perf-smoke.sh` gated on
   `perf.available()`.
5. **Docs**: measurement-discipline guide in this note; flip status to
   complete.

## Verification

- Instruction-count ground truth: a counted loop of K instructions per
  iteration × N iterations → `instructions ≈ N·K` within 1%.
- IPC contrast: a dependent-chain loop (IPC ≈ 1) vs. independent
  operations (IPC ≫ 1).
- Cache contrast: pointer-chase over a buffer ≫ LLC (misses ≈ chase
  length) vs. a buffer ≪ L1 (misses ≈ 0).
- Branch contrast: a data-dependent unpredictable branch vs. a counted
  loop.

## Risks and scope limits

- KVM vPMU is reliable for **counting**; sampling, PEBS, and LBR are out
  of scope (and unnecessary for this lab).
- Hyper-threading halves the GP counters; QEMU vCPUs expose no HT, and on
  the XPS the count is read from CPUID rather than assumed.
- TCG (CI) has no PMU: everything degrades to the current TSC-only
  behavior behind `perf.available()`.
