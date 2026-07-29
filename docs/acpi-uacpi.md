---
title: ACPI — uACPI integration
tags: [design, acpi, in-progress, real-hardware]
status: in-progress
related: ["[[x86-64-port]]", "[[Index]]"]
created: 2026-07-28
---

# ACPI — integrating uACPI

> [!abstract] Goal
> Replace the kernel's hand-rolled ACPI layer with
> [uACPI](https://github.com/uACPI/uACPI) — a portable, freestanding ACPI
> implementation with a real AML interpreter — following the incremental path
> below.

> [!success] Status — **Steps 1–2 landed** (b54b4cf, 50606f0). uACPI 6.0.0 is
> vendored in **barebones mode** (tables only, no AML) and `acpi.c` now parses
> the FADT/MADT/DSDT through it — the hand-rolled RSDP/SDT walk is gone (−160
> lines). Interrupts (IOAPIC), PM timer, and _S5 shutdown all run off uACPI.
> Step 3 (full init: AML → _S5 via `enter_sleep_state`, _PRT, power button) is
> next and is the big lift.

## What we have today (`src/acpi.c`, ~420 lines)

Pure **static-table parsing**, no AML interpreter:

- RSDP → RSDT/XSDT walk.
- **FADT**: PM1 control port, PM timer (`acpi_pm_timer_port`), reset register.
- **MADT**: LAPIC base, first IOAPIC, IRQ→GSI interrupt-source overrides
  (`acpi_lapic_base` / `acpi_ioapic` / `acpi_irq_to_gsi`) — what the APIC
  bring-up needs.
- Shutdown (`acpi_shutdown`): a **hand-rolled byte-scan of the DSDT for the
  `_S5_` package** to extract `SLP_TYPa/b`, then a PM1 write. This is
  pattern-matching raw AML bytes, not interpreting them.

No namespace, no AML evaluation, no `_PRT`, no GPE/SCI events, no device power
management.

> [!warning] Known-fragile piece
> The `_S5_` byte-scan works on QEMU's simple DSDT but is exactly what breaks on
> real firmware (where `_S5` may live inside a method, be encoded differently,
> or depend on other objects). This is the first thing to replace.

## What uACPI is

A freestanding full ACPI stack: AML interpreter + namespace + table management +
event/GPE handling + sleep-state / power management + PCI routing + resource
(`_CRS`) parsing. The OS supplies a ~15-callback "kernel API" glue layer:

- physical memory map/unmap — have (`iomap` / `paging`)
- PCI config read/write — have (`pci_read32/write32`; need byte/word + ECAM)
- I/O port in/out — have (`ports.h`)
- alloc/free — have (heap)
- log — have (`console`)
- stall (busy) + sleep (ms) — have (`ktime`; blocking sleep via the scheduler)
- spinlock / mutex / event create-acquire-release — spinlock exists; mutex/event
  are easy stubs on a single-CPU cooperative kernel
- get RSDP — have (from Limine)
- install the SCI interrupt handler — have (`idt` + `ioapic_route`)
- **schedule deferred work** — new plumbing (GPE handling defers out of the SCI);
  the cooperative scheduler can back it

Freestanding fit is good (no libc; you provide the platform layer), consistent
with the project's existing vendoring (Lua, BearSSL, flanterm, microui) — though
it would be the largest such dependency (~10–15k LOC, AML interpreter).

## What it buys *this* kernel

Ordered by relevance to the real-hardware (XPS) goal:

1. **Reliable shutdown** — `uacpi_prepare_for_sleep_state(S5)` /
   `uacpi_enter_sleep_state` evaluates real AML, deleting the brittle `parse_s5`
   scan.
2. **PCI interrupt routing (`_PRT`)** — the correct source for legacy INTx GSI +
   polarity. Touches current work: the [[Index|AC'97 audio]] IRQ milestone and
   any real-HW INTx device. (On QEMU the PCI Interrupt Line register suffices
   because firmware fills it; `_PRT` is the right answer on the XPS.)
3. **Power button / lid / SCI events** — graceful shutdown, lid handling; only
   obtainable via GPE + SCI, i.e. an AML runtime.
4. **Embedded Controller + battery / thermal** — laptop status lives behind the
   EC and AML methods.
5. **Robust table discovery + MCFG/HPET** — tested discovery replaces the
   hand-rolled walk; MCFG unlocks PCIe **ECAM** (extended config space the XPS
   uses), HPET a better timer.

## Costs

- Large vendored dependency (AML interpreter): code size + attack surface, and a
  philosophical dent in the "from scratch" ethos (build with `-w`, like
  lua/bearssl).
- Glue must be written carefully; **GPE/SCI wants deferred work** — new plumbing
  atop the cooperative scheduler.
- We'd delete/shrink the `_S5` byte-scan and the hand-rolled SDT walk — a net
  simplification in exchange for the dependency.

## Recommendation & trigger

**Not needed for QEMU.** Static parsing already covers APIC topology, PM timer,
and PM1 shutdown there. **The tipping point is committing to the real XPS as a
daily target**: graceful shutdown, power button, battery/thermal via EC, correct
IRQ routing, and robust firmware handling all become real needs — and
hand-writing an AML interpreter is precisely the rabbit hole uACPI avoids.

## Incremental path

1. **[done]** Vendor uACPI + glue in barebones mode (`UACPI_BAREBONES_MODE` +
   `UACPI_USE_BUILTIN_STRING`; four callbacks: get_rsdp / map / unmap / log),
   `uacpi_setup_early_table_access`, boot report. Runs beside the hand-rolled
   parser. (`src/uacpi/`, `src/uacpi_glue.c`.)
2. **[done]** `src/acpi.c` parses via uACPI table lookups
   (`uacpi_table_fadt` / `uacpi_table_find_by_signature`) using uACPI's typed
   structs; the hand-rolled RSDP/SDT walk + table structs + `map_phys` are gone.
   The `_S5` byte-scan stays (soft-off needs AML — deferred to step 3). Public
   `acpi.h` API unchanged. MCFG (ECAM) / HPET are now trivially reachable too.
3. Drop barebones and do full `uacpi_initialize` + namespace load (needs the
   larger glue: alloc, mutex/event/spinlock, io/pci, interrupts, deferred work)
   → proper `_S5` shutdown (`uacpi_prepare_for_sleep_state` / `enter_sleep_state`)
   and `_PRT` PCI IRQ routing (feeds the audio INTx + real-HW INTx).
4. Later: SCI power button, EC, battery/thermal, CPU C-states.

Note: step 3's full init is the big lift (many more callbacks, and the AML
interpreter compiled in — drop `UACPI_BAREBONES_MODE`). Steps 1–2 are pure
robustness wins with the minimal glue.

The most concrete near-term payoff that intersects current work is **`_PRT` for
interrupt-driven drivers on real hardware**; the biggest strategic one is **not
hand-writing an AML interpreter** once the XPS's firmware needs one.
