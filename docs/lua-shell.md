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
- **M8 — SMP.** Limine MP request to start the application processors; per-CPU
  GDT/TSS/IDT, per-CPU data, LAPIC, and spinlocks (the kernel's `cli`-as-lock
  assumption no longer holds once APs run).
- **M9 — Parallel Lua library.** A `lua_State` per core, `thread.spawn(core, fn)`
  dispatch, `mem.shared` shared buffers, and `mem.phys`/`ncores()` primitives —
  the custom multithreading library.

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
the `k` library measures. `run("demo.lua")` draws a sampler, and
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

## The `pci` library — PCI configuration space

`pci.read(bus,dev,func,offset)` / `pci.write(...)` access config space (via the
0xCF8/0xCFC mechanism), and `pci.list()` returns a table of every device
(`{bus, dev, func, vendor, device, class, subclass, prog_if, header}`).
`run("lspci.lua")` prints them lspci-style with class names — the prerequisite
introspection for future device drivers.

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
`build/scripts/init.lua` runs once at startup — edit it and rebuild to customize
the boot. The REPL handles multi-line input: an incomplete statement continues
with a `>>` prompt, so whole snippets can be pasted.

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
