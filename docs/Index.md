---
title: Index
tags: [moc, index]
---

# juampiOS — design vault

Map of content for the juampiOS design notes. This folder is an
[Obsidian](https://obsidian.md) vault: notes use `[[wikilinks]]`, YAML
frontmatter properties (`tags`, `status`, `milestone`), and callouts. Open the
`docs/` folder as a vault to get the graph view and backlinks.

## Notes

- [[x86-64-port]] — migrating the kernel from 32-bit protected mode to 64-bit
  long mode under Limine. *Status: complete.*
- [[lua-shell]] — booting into a parallel, ring-0 Lua shell with per-core
  interpreters and shared memory. *Status: complete.*
- [[networking]] — TCP/IP stack design: e1000 driver, polled execution model,
  Ethernet/ARP/IPv4/ICMP/UDP, DHCP/DNS, and a `net.*` socket library.
  *Status: planned (M10).*
- [[ui]] — desktop UI & windowing on microui: what was built, the global-state
  problem, and the reentrant (later multithreaded) target design.
  *Status: in progress (M11).*
- [[reentrancy-audit]] — whole-kernel review of mutable state, reentrancy, SMP
  safety, allocator discipline, and the UI API (Lua + ELF), with a fix plan.
  *Status: in progress (M11).*
- [[constants-audit]] — sweep for magic numbers, hardcoded offsets, and
  hand-picked MMIO addresses, with a prioritized fix order.
  *Status: in progress (M11).*
- [[acpi-uacpi]] — follow-up proposal: swap the hand-rolled table-only ACPI
  layer for uACPI (AML interpreter) when the real XPS becomes a firm target.
  *Status: in progress (full AML init + power button + _PRT).*
- [[hda-audio]] — Intel HD Audio backend: MMIO, CORB/RIRB verbs, codec
  enumeration, stream DMA — the modern/real-XPS audio path behind the mixer
  vtable. *Status: complete in QEMU; real-HW headphone path deferred.*
- [[hosted-libc]] — run ordinary newlib-linked C programs in ring 0 via an
  int-0x80 syscall layer (stdio, malloc, math, ext2 file I/O). *Status: complete
  — curated newlib subset vendored + built by `make`.*
- [[api-layers]] — review of the API surfaces (Lua bindings, lab API table,
  hosted syscall ABI + juampi platform lib, the two libcs): how each works,
  the decisions behind them, and a prioritized friction/improvement list.
  *Status: review.*
- [[doom-port]] — walkthrough of the Doom port: architecture, design
  decisions, kernel changes, the syscall/platform APIs it introduced, and the
  vendored-engine delta. *Status: complete.*
- [[perf-lab]] — the Intel PMU (fixed + architectural events) as a `perf.*`
  Lua module and an upgraded `bench()` for noise-free measurement, over the
  q35/`-cpu host` QEMU realism work. *Status: complete.*
- [[software-rasterizer]] — graphics roadmap: a shared software 2D rasterizer
  as the keystone, the tracks on top (fonts, windowing, 3D, image formats, Lua
  canvas), quick wins, and the raylib connection. *Status: planned (fonts +
  quick wins in progress).*

## By status

| Note                 | Status            | Milestone |
| -------------------- | ----------------- | --------- |
| [[x86-64-port]]      | complete          | —         |
| [[lua-shell]]        | complete          | M8–M9     |
| [[networking]]       | planned           | M10       |
| [[ui]]               | in progress       | M11       |
| [[reentrancy-audit]] | in progress       | M11       |
| [[constants-audit]]  | in progress       | M11       |
| [[acpi-uacpi]]       | in progress       | —         |
| [[hda-audio]]        | complete (QEMU)   | —         |
| [[hosted-libc]]      | complete          | —         |
| [[api-layers]]       | review            | —         |
| [[doom-port]]        | complete          | —         |
| [[perf-lab]]         | complete          | —         |
| [[software-rasterizer]] | planned        | —         |

## Conventions

- **Frontmatter** — every note carries `title`, `tags`, and (for feature work)
  `status` + `milestone`.
- **Links** — relate notes with `[[note-name]]` (filename without `.md`).
- **Callouts** — `> [!abstract]` goals, `> [!warning]`/`> [!danger]` gotchas,
  `> [!note]` asides.
