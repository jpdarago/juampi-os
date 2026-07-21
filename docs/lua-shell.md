---
title: Lua shell
tags: [design, lua, smp, parallel, complete]
status: complete
milestone: M8–M9
related: ["[[x86-64-port]]", "[[networking]]", "[[Index]]"]
---

# Booting to a parallel Lua shell

Goal: boot into an interactive Lua shell over the serial console, with a custom
library for multithreaded programming that has full access to all CPU cores and
all of physical memory.

The shell and the interpreters run in **ring 0** (kernel mode). That is what
gives "full access to all cores and all memory" for free: no syscall boundary,
Lua code can touch any physical address (via the HHDM) and dispatch work to any
core directly. This is a single-user, single-address-space machine by design —
a parallel programming appliance, not a protected multi-user OS.

## Threading model: per-core states + shared memory

Chosen model (à la Lua Lanes / SPMD): **one independent `lua_State` per core**,
running truly in parallel. `thread.spawn(core, fn)` serialises `fn` (as bytecode)
to a target core, which runs it in its own interpreter. Cores share data through
an explicit **shared-memory API** (`mem.shared(n)` → raw bytes every core can
read/write), never by sharing `lua_State`s (which are not thread-safe).

```lua
shared = mem.shared(1 << 20)          -- 1 MiB every core sees
for c = 1, ncores()-1 do
    thread.spawn(c, function()
        local base = c * (len // ncores())
        shared:u64(base, compute(base))
    end)
end
```

## Milestones (each a bootable, `make test`-validated commit)

- **M6 — Serial shell.** COM1 receive path + a line editor + a kernel command
  loop (builtins). The interactive endpoint the rest builds on.
- **M7 — Lua port.** ✅ DONE. Vendored PUC-Lua 5.4.8 (`src/lua/`), built
  freestanding over a small libc shim (`src/lua/klibc/` stub headers +
  `klibc.c` string/stdlib/stdio, `klibc_math.c` x87/SSE math,
  `klibc_setjmp.asm`), backed by the kernel heap (`malloc`/`realloc`/`free`) and
  console (`fwrite`/`printf`). `io`/`os`/`package`/`loadlib` are stripped; the
  glue (`lua_glue.c`) opens base/string/table/math/coroutine and runs a line at
  a time like the standard REPL. `shell.c` is now the Lua shell. Number
  formatting uses the vendored float printf; FP relies on the SSE/x87 work.
  Validated: `print(math.sqrt(2))`, loops, closures, tables, string library,
  and Lua error messages all work over serial and the PS/2 keyboard.
- **M8 — SMP.** ✅ DONE. Limine MP request starts the application processors;
  each gets its own GDT/TSS (shared IDT) and per-CPU data (via `%gs`); a
  spinlock (`include/spinlock.h`) is the first concurrency primitive; work is
  dispatched over a spin-polled per-CPU mailbox (`smp_run_on`/`smp_join`, no
  LAPIC/IPI needed). `src/smp.c`, `src/smp_ap.S`.
- **M9 — Parallel Lua library.** ✅ DONE. One `lua_State` per core, each with its
  own heap so allocation is lock-free (`src/lua/lua_thread.c`); the `thread` and
  `mem` libraries below. See the `thread`/`mem` section.

## The `k` library — kernel introspection from Lua

Because the shell runs in ring 0, Lua has full access to the machine, exposed
through a `k` library:

- **Time / profiling:** `k.rdtsc()`, `k.ns/us/ms()`, `k.uptime()`, `k.tsc_hz()`
  (benchmarking is the top-level `bench()` — see "Running scripts").
- **SMP:** `k.ncores()`, `k.cpu()` (the core the shell runs on).
- **Memory / CPU:** `k.freemem()`, `k.totalmem()`, `k.freeframes()`,
  `k.cpuid(leaf [,sub])`, `k.cpubrand()`, `k.rdmsr/wrmsr(msr [,val])`.
- **Raw access:** `k.peek8/16/32/64(addr)`, `k.poke8/16/32/64(addr,val)`,
  `k.inb/outb(port [,val])`, `k.hexdump(addr [,len])`.
- **Power / entropy:** `k.shutdown()` (ACPI S5 power-off) and `k.reboot()` parse
  the firmware's ACPI tables (via Limine's RSDP), so they work on real hardware,
  not just QEMU; `k.random()` returns a hardware random integer from `RDRAND`
  (TSC-seeded PRNG fallback if the CPU lacks it).
- **Symbolication:** `k.sym(addr)` → name, offset; `k.backtrace()`.

Poking a bad address or MSR faults into the (symbolized) exception handler, but
that no longer halts the machine: while the shell is evaluating a script it arms
fault recovery, so a bad `k.poke`/`k.rdmsr` unwinds back to the prompt (resetting
the interpreter) instead of panicking. This turns the shell into a live kernel
explorer, profiler, and prototyping surface you can poke at freely.

## The `fb` library — framebuffer graphics from Lua

`fb.width/height()`, `fb.pixel(x,y,rgb)`, `fb.rect(x,y,w,h,rgb)`,
`fb.line(x0,y0,x1,y1,rgb)`, `fb.clear(rgb)` draw directly to the Limine
framebuffer (colours are `0xRRGGBB`). It shares the surface with the text
console, so graphics and text overwrite each other — good for *visualizing* what
the `k` library measures.

`fb.setmode(w,h)` changes the display resolution at runtime (32bpp, via the QEMU
stdvga Bochs-DISPI registers), re-pointing both graphics and the console at the
new geometry. `fb.buffer(true)`/`fb.flip()` add double buffering (see
`boing.lua`). For parallel rendering, `fb.canvas()` returns a `mem.shared`-style
handle aliasing the live framebuffer and `fb.pitch()`/`fb.shifts()` give its
stride and channel layout, so **every core can write disjoint pixels straight to
the screen**: `run("raytracer.lua")` traces a sphere scene across all cores with
`thread.parallel`, filling the framebuffer band by band.

`run("demo.lua")` draws a sampler, and
`run("boing.lua")` plays the Amiga "Boing Ball" (a shaded checker sphere
bouncing over the magenta grid) as an animation/stress test of the `fb` library.

`fb.image(name [,x,y [,key [,tol]]])` decodes a **QOI** image (`src/qoi.c` — a
~90-line decoder for the "Quite OK Image" format) shipped as a Limine module and
blits it, skipping transparent pixels; `x`/`y` default to centring it. Pass
`key` (an `0xRRGGBB` colour) to chroma-key it out — pixels within `tol`
(default 16) per channel become transparent — so a flat background drops away.
The boot logo (`build/scripts/logo.qoi`, drawn top-right by the shell and kept
redrawn after each command, or shown anywhere with `run("logo.lua")`) is a
checked-in asset, so the normal build needs no image tooling. Regenerate it from
the source art (`build/assets/logo.png`) with `make logo`: ImageMagick resizes
it, flood-fills the connected background to transparent (so it floats over the
console rather than showing as a square, without touching the enclosed globe),
and the `png2qoi` host tool encodes the RGBA with the reference QOI codec — so
the kernel decoder is validated against a real, independently-encoded file
(confirmed pixel-identical).

## The `fs` / `disk` libraries — the ext2 data disk

A second QEMU disk (`disk.img`, attached as the primary IDE slave; built from
`build/disk/` with `mke2fs`) carries an **ext2** filesystem that a polled ATA PIO
driver (`src/ata.c`) reads and writes.

- `disk.present()`, `disk.sectors()`, `disk.read(lba[,count])` — raw 512-byte
  block access.
- `fs.read(path)`, `fs.list(path)`, `fs.stat(path)`, `fs.exists(path)`,
  `fs.mounted()` — read the filesystem (superblock, inodes, direct + single/
  double-indirect blocks, directory walk).
- `fs.write(path, data)`, `fs.mkdir(path)`, `fs.remove(path)` — **write** support
  (`src/ext2.c`): create/overwrite files (direct + single indirect, ~268 KiB),
  make directories, and delete files/empty directories, keeping the superblock,
  group descriptors, and bitmaps consistent (verified with host `e2fsck`). No
  journal — a crash mid-operation is `e2fsck`-recoverable.

`run("name")` also loads scripts and binaries off this disk (see below), so you
can write a script to the disk and run it: `fs.write("/x.lua", '...'); run("/x.lua")`.

## The `pci` library — PCI configuration space

`pci.read(bus,dev,func,offset)` / `pci.write(...)` access config space (via the
0xCF8/0xCFC mechanism), and `pci.list()` returns a table of every device
(`{bus, dev, func, vendor, device, class, subclass, prog_if, header}`).
`run("lspci.lua")` prints them lspci-style with class names — the prerequisite
introspection for future device drivers.

## The `thread` / `mem` libraries — parallel Lua

The multithreading library (M9). Each core runs an independent `lua_State` with
its **own heap**, so cores allocate without a lock; work crosses cores as an
explicit function + arguments (never a shared `lua_State`), and cores share data
through `mem.shared` buffers.

- `thread.cores()`, `thread.cpu()` — core count and the current core;
  `thread.rdtsc()`, `thread.ns()` — timing inside a worker.
- `thread.spawn(core, fn, ...args)` / `thread.join(core)` — run `fn(core, ...)`
  on an application processor and collect its result. `fn` is serialized to
  **bytecode**, which carries no upvalues, so it must take its inputs as
  arguments — a function that captures a local is rejected with a clear error.
- `thread.parallel(fn, ...args)` — the SPMD helper: run `fn(cpu, ...)` on every
  core, join, and return a per-core results table. A worker that errors makes it
  re-raise (recovering to the prompt, not hanging).
- `mem.shared(n)` → a zeroed buffer every core can read/write, with bounds-checked
  accessors `buf:size()` and `buf:u8/u16/u32/u64/f64(off [,v])` (get if `v`
  omitted, else set). Pass a buffer as a spawn arg and every core sees the same
  memory. Args and results are limited to nil/boolean/number/string/shared-buffer.

Worker states are compute-only (base/string/table/math/coroutine + `thread`/`mem`);
hardware libraries (`fb`/`disk`/`fs`/`pci`/`k`) are the shell's, to avoid cores
racing on shared devices. `run("parallel.lua")` sums a shared array across all
cores and checks it against the serial sum, reporting the speedup.

## Running and benchmarking code

Two polymorphic globals launch and measure code regardless of language:

- **`run(name [,arg])`** loads by name — a built-in Limine module first, then the
  ext2 disk (`name`, `/name`, `/scripts/name`, `/lab/name`) — and dispatches on
  the bytes: a `.lua` script is executed (receiving `arg` as a vararg); an ELF64
  binary is loaded and **called directly in ring 0**, returning its result.
- **`bench(target [,arg=0] [,iters=1000])` → `total_cycles, per_call`** times a
  Lua function, a script, or a native binary the *same* way (TSC-fenced calls of
  `target(arg)`), so implementations are directly comparable across the language
  boundary — e.g. `bench("quick.elf", 3000)` vs `bench(my_lua_sort, 3000)`.

Lua scripts are shipped as Limine modules (files under `build/scripts/`);
`build/scripts/prelude.lua` runs first (built-in helpers), then
`build/scripts/init.lua` (edit it and rebuild to customize the boot). The REPL
handles multi-line input: an incomplete statement continues with a `>>` prompt,
so whole snippets can be pasted.

Shell conveniences: the line editor recalls previous commands with the **up/down
arrows** (history, working over both serial and the PS/2 keyboard); `clear()`
clears the screen; `help()` prints an overview and `help(lib)` (e.g. `help(fb)`)
lists a library's functions; and `dump(v)` / `pp(v)` pretty-print a value or
table (indented, sorted keys, cycle-safe). `help`/`dump` live in `prelude.lua`.

**Native binaries (the "lab").** C programs in `build/lab/` are compiled
freestanding, statically linked at a fixed VA, and shipped as Limine modules. A
binary's entry is `long bench(const lab_api* api, long arg)`; the kernel
(`src/lab.c`) hands it a table of callbacks (`alloc`/`free`, `print`,
`rdtsc`/`ns`, `ncores`, `run_on`/`join` — see `include/lab.h`) and calls it in
ring 0 for the cleanest possible timing. Because the kernel enforces no NX/SMEP,
the loaded blob is directly callable; a fault inside it unwinds to the prompt via
the shell's fault recovery, so a buggy benchmark can't halt the machine. This is
a "sterile lab" for pitting algorithm implementations against each other —
`build/lab/insertion.c` vs `build/lab/quick.c` return the same checksum with very
different cycle counts.

## What this deliberately gives up

Protection: Lua scripts run in ring 0 and can crash or corrupt the machine.
That is the explicit trade for "full access to all cores and all memory." The
ELF64 user-mode path from the port (ring 3 + `int 0x80`) stays in the tree for
when isolation is wanted, but the Lua shell does not use it.
