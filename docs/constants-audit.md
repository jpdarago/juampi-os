---
title: Hardcoded-constants audit
tags: [audit, cleanup, magic-numbers, needs-work]
status: in progress
milestone: M11
related: ["[[reentrancy-audit]]", "[[Index]]"]
---

# Hardcoded-constants audit

> [!abstract] Scope
> A whole-kernel sweep for magic numbers, hardcoded memory offsets, and
> hand-picked addresses (prompted by the reviewer noticing "a lot of hardcoded
> constants" in the recent APIC/ACPI work). Goal: separate the genuine hazards
> from the values that are fine as named constants, and give a fix order.

## Bottom line

The codebase is **mostly disciplined**: wire formats use packed structs
(`net`/`tcp`/`udp`/`ext2`/`elf64`, e1000 descriptors) and device registers are
named `#define`s (e1000/ata/serial/pci/apic). The real problems are concentrated
in a few places — several of them recent APIC/ACPI code.

## Severity legend

- **HAZARD** — fragile/error-prone or collision-prone; worth fixing.
- **DEDUP** — same constant defined in N places; a maintenance trap.
- **POLISH** — a stable spec value used raw; naming it aids readability, low
  urgency.
- **FINE** — already a named constant / packed struct / tunable knob; leave it.

## 1. HAZARD — ACPI tables parsed by raw byte offsets (no structs)

`src/acpi.c` reaches into the FADT/MADT with bare offsets — `f + 64` (PM1a),
`f + 112` (flags), `f + 140` (X_DSDT), `f + 208` (X_PM_TMR), `m + 36` (LAPIC
base), `off + 4/8` (MADT entries) — with **no `struct fadt`/`struct madt`**.
This is the most error-prone magic (a wrong offset = silent misparse of
spec-critical data). Same, smaller: `dns.c` (entirely hand-indexed — `m[o+8]`,
`0xC0` compression, no header struct) and `qoi.c` (14-byte header via `p[12]`,
`be32(p+4)`). **Fix: packed structs**, matching what `ext2.c`/`elf64.c` already
do well. → *fix (b2) for the FADT/MADT (`ce17bf2`); dns.c + qoi.c done in
`f262b94`.*

## 2. HAZARD — MMIO virtual-address windows are hand-assigned

Five higher-half windows are picked by hand with no registry:

| Window | VA | Size | File |
| ------ | -- | ---- | ---- |
| KHEAP  | `0xffffc00000000000` | 128 MiB | paging.h |
| FBWIN  | `0xffffe00000000000` | 16 MiB  | gfx.c |
| NICWIN | `0xffffe00002000000` | 128 KiB | e1000.c |
| LAPIC  | `0xffffe00003000000` | 4 KiB   | apic.c |
| IOAPIC | `0xffffe00003001000` | 4 KiB   | apic.c |

They don't overlap *today*, but nothing enforces it — the next driver picks
another literal and hopes. (Two of these were added in the APIC work.) **Fix: an
`iomap(phys, len) -> va` bump allocator** over one MMIO region that maps
uncached and hands back a VA, retiring the four device windows (KHEAP stays
separate — it's the heap, not device MMIO). → *fix (b1)*

## 3. DEDUP — the same constant in N places (easy wins) — ☑ done

> [!note] Resolved in `54ae5c9` — see the four bullets below; all now source
> from `include/font.h`, `include/theme.h`, `include/lineedit.h`, and
> `KERNEL_STACK_SZ` in `include/sched.h`.


- **Glyph size `8×16` in FOUR files:** `FONT_W/H` (font8x16.h), `GLYPH_W/H`
  (ui.c), `VGW/VGH` (editor.c), `GW/GH` (term.c). → all derive from `FONT_*`.
- **`LINE_MAX=256` + `HIST_MAX=32`** in both `shell.c` and `term.c`. → one header.
- **Kernel stack `0x4000`:** `STACK_SZ` (sched.c) + `AP_STACK_SZ` (smp.c). → one
  name.
- **Theme colours scattered:** the 5-entry highlighter palette
  (`0xd4d4d4,0x6ac46a,…`) is in both `term.c` and `ui.c`; `0x0e1116` (bg) and
  `0x9ecbff` (cursor) recur in `term.c`+`editor.c`. → one `theme.h`.

## 4. POLISH — name the magic (readability, not correctness)

Stable spec values used raw; often commented, so low urgency:
- **Ports:** DHCP 67/68, DNS 53, HTTP/HTTPS 80/443; DHCP magic cookie
  `0x63825363`, BOOTREQUEST/REPLY 1/2.
- **IDs/magic:** PCI vendor/device `0x8086`/`0x100E` (e1000), ELF `0x7F 'ELF'` +
  `ELFCLASS64`, QOI hash coeffs `3,5,7,11`.
- **Bit-packed config as one literal:** GDT descriptors
  (`0x00AF9A000000FFFF`…), serial UART setup (`0x03`,`0xC7`,`0x0B`), i8042
  commands (`0xD4`,`0xA8`,`0x60`), e1000 `TIPG=0x0060200A`.
- CPUID leaves (`0x15`/`0x16`/`0x80000007`), the x2APIC `0x800 + off/16` math,
  page-offset masks (`0x1FFFFF`,`0x3FFFFFFF`).

## 5. FINE — leave alone

- **Named device register offsets** (e1000 `REG_*`, ata, serial, pci ports, apic
  `REG_*`) — this *is* the right pattern.
- **Packed wire-format structs** (net/tcp/udp/ext2/elf64).
- **Arbitrary capacity caps** (`MAX_THREADS`, `TCP_CONNS`, `TROWS/TCOLS`,
  `ED_MAX_LINES`, arena sizes, `WORKER_HEAP_SZ`) — tunables, already named, and
  tracked in [[reentrancy-audit]] §3. Not magic, just knobs.

## Fix order

1. ☑ **(b1)** `iomap()` VA-window allocator — retires the four device VAs +
   prevents the next collision. *(commit `da2bb4e`)*
2. ☑ **(b2)** `struct fadt`/`struct madt` in acpi.c — owns the worst offender
   (and it's recent code). *(commit `ce17bf2`)*
3. ☑ DNS/QOI header structs (same family as b2, smaller). *(commit `f262b94`)*
4. ☑ The dedup pass (§3) — `font.h`, `theme.h`, `lineedit.h`, and
   `KERNEL_STACK_SZ` in `sched.h`. Pure refactor. *(commit `54ae5c9`)*
5. ☐ Opportunistic naming (§4).

## Related

[[reentrancy-audit]] (the allocation/capacity side), [[Index]].
