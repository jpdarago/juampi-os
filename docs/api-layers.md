---
title: API layers
tags: [design, review, api, lua, syscall, libc, hosted, lab]
status: review
related: ["[[hosted-libc]]", "[[lua-shell]]", "[[reentrancy-audit]]", "[[Index]]"]
created: 2026-07-31
---

# API layers — a review

> [!abstract] Goal
> A review pass over how code *outside the kernel proper* talks to the kernel:
> the *Lua bindings*, the *lab API table*, the *hosted syscall ABI* + the
> *juampi platform C lib*, and the *two libcs*. What each layer is, how it's
> implemented, the decisions that shaped them, and where the friction is.

## The one-sentence summary

Everything runs in **ring 0** in a **single address space**, so none of these
layers is a *protection* boundary — they are three deliberately different
**coupling** boundaries: Lua links *into* the kernel, lab programs receive a
*table of kernel function pointers*, and hosted programs see *only a syscall
ABI*. The tradeoff each makes is API richness vs. independence from the kernel
image.

## Layer map

```
        ┌────────────────────────────────────────────────────────┐
        │                     kernel (ring 0)                    │
        │  subsystems: gfx audio net ext2 smp ktime acpi pci …   │
        └───────┬──────────────────┬──────────────────┬──────────┘
        direct calls        struct lab_api      int 0x80 dispatch
                │            (fn pointers)       (src/syscall.c)
        ┌───────┴───────┐   ┌──────┴───────┐   ┌──────┴─────────────┐
        │ Lua bindings  │   │ lab programs │   │ hosted programs    │
        │ src/lua/lua_* │   │ build/lab/*  │   │ build/hosted/*     │
        │ 13 modules +  │   │ entry: bench │   │ entry: _start→main │
        │ luadoc        │   │ no libc      │   │ newlib + libgloss  │
        └───────────────┘   └──────────────┘   │ + juampi.h platform│
                                               └────────────────────┘
```

| | **Lua** | **Lab** | **Hosted** |
|---|---|---|---|
| Boundary | none (in-process) | API struct passed to entry | `int 0x80` (rax nr, rdi/rsi/rdx args, rax ret, `-errno`) |
| Entry | REPL / `run("x.lua")` | `long bench(const struct lab_api*, long)` | crt0 `_start` → `main(argc, argv)` |
| Kernel API surface | ~13 modules, ~100+ fns | 14 fn pointers (`include/lab.h`) | 21 syscall numbers (0–20) |
| libc | klibc shim (`src/lua/klibc/`) | none (freestanding) | vendored newlib subset (`src/newlib/`) |
| SMP | `thread.parallel` | `api->parallel` / `run_on`/`join` | none |
| Files | `fs.*` (ext2) | none | open/read/write/… (ext2, whole-file buffered) |
| Graphics | `fb.*` (draw calls) | raw fb pointer + pitch/shifts | `juampi_fb_present` (blit a frame) |
| Detection | not ELF | ELF without `_start` | ELF with `_start` symbol |
| Typical user | the shell, scripts, demos | SMP benchmarks, raytracer | Doom, chello, filetest |

## How each layer works

### 1. Lua bindings (`src/lua/lua_*.c`)

Thirteen modules (`k, fb, pci, disk, fs, thread, mem, net, http, ui, input,
usb, audio`) plus globals (`run`, `bench`, `edit`, `clear`, `quit`). Bindings
are **thin synchronous wrappers that call kernel subsystems directly** —
`audio.tone` → `audio_tone()`, `fs.read` → `ext2_read_path()`. No indirection
layer exists or is needed: the interpreter *is* kernel code.

The registration mechanism is the nicest piece of API engineering in the tree:
`struct lua_fndoc` (`include/luadoc.h:14`) couples each function pointer with
its docstring and typed arg/return descriptors, and `luadoc_newlib()`
(`src/lua/luadoc.c:29`) registers the module *and* builds a `__doc` sidecar
table — the shell's `help()` is generated from the same array that registers
the functions, so docs cannot drift from the surface.

Objects with identity (sockets, shared buffers) use standard metatables with
`__gc` finalizers (`lua_net.c:413`, `lua_thread.c:635`).

Error convention: *programmer errors* (bad types, out-of-bounds) throw via
`luaL_error`; *runtime conditions* (no device, missing file, no free voice)
return `nil, "message"`. Mostly — see friction §F5.

### 2. Lab programs (`include/lab.h`, `src/lab.c`)

A lab binary is freestanding C with one entry point, `bench(api, arg)`. The
kernel passes `struct lab_api`: alloc/free, print, rdtsc/ns, and the real
payload — **SMP** (`run_on`/`join`/`parallel`) and the **raw framebuffer**
(pointer, pitch, channel shifts). This is a *capability table*: the program
never links against kernel symbols, so lab binaries survive any kernel
relayout; the ABI is the struct layout (append-only by convention).

`lab_run()` (`src/lab.c:143`) maps the ELF at its fixed link address
(0x400000) via `elf64_load_exec` and calls `bench` as a plain function call.
Faults unwind to the shell through the kernel's `fault_recover` path.

### 3. Hosted programs (`src/syscall.c`, `build/hosted/`)

The full-libc model: `crt0.S` (37 lines) → `.init_array` → `main` → `exit`,
with newlib's reentrant layer calling bare-named stubs (`write`, `sbrk`, …) in
`build/hosted/syscalls.c` (248 lines), each a 3-arg `int $0x80` trap. The
kernel dispatcher (`src/syscall.c`) implements 21 numbers:

- **POSIX core (0–9):** exit, read, write, open, close, lseek, fstat, (isatty
  reserved but resolved in libgloss), sbrk, gettimeofday. File I/O is
  *whole-file buffered* in the kernel heap and flushed to ext2 on close;
  `fstat` returns just the size as a scalar and libgloss builds `struct stat`
  — the kernel never learns newlib's struct layouts.
- **juampi platform (10–20), declared in `build/hosted/juampi.h`:** fb_info,
  fb_present (blit + first-call fullscreen takeover), getkey (raw PS/2
  make-codes from a second keyboard ring), ticks_ms, audio_play/stop/active
  (Doom SFX), audio_music_start/write/space/stop (the OPL streaming path).

`hosted_run()` (`src/syscall.c:470`) loads the ELF (also `elf64_load_exec`),
carves a 16 MiB sbrk heap, `setjmp`s an exit environment, and calls `_start`.
`SYS_exit` longjmps back; teardown closes FDs, returns the screen to the
compositor, and drains the keyboard rings. One program at a time, on the
caller's stack.

The eleven process-model calls (fork, execve, stat, times, …) are **local
errno stubs** in syscalls.c and never trap — the "no process model" decision
made explicit at the libc edge rather than in the kernel.

### 4. The two libcs

- **klibc** (`src/lua/klibc/`, ~470 LOC + headers): the minimal freestanding
  shim the vendored Lua needs — string/stdlib bits over the kernel heap,
  setjmp in asm, stdio routed to the kernel's embedded printf
  (`src/printf/printf.c`, shared with all kernel code).
- **newlib** (`src/newlib/`, 107 vendored .c files ≈ 2.2 MB source → 329 KB
  archive): the real libc for hosted programs, curated by linker map
  (`vendor-newlib.sh`), compiled with `-D_LIBC -DMISSING_SYSCALL_NAMES
  -DHAVE_MMAP=0` so the reentrant layer calls our bare-named stubs and malloc
  is sbrk-only.

They are intentionally disjoint: the kernel (and Lua) never link newlib;
hosted programs never see kernel headers. The only shared *idea* is printf —
two independent implementations (embedded printf in-kernel, newlib's vfprintf
for hosted), which is duplication of function but not of coupling.

## The main decisions along the way

> [!note] Reconstructed from the code, the docs ([[hosted-libc]],
> [[lua-shell]]) and the git history; ordered roughly chronologically.

1. **Ring 0 everywhere, one address space.** The founding tradeoff: no
   isolation, maximal simplicity and performance. Every later layer is shaped
   by it — "syscalls" exist for *decoupling*, not protection.
2. **Lua as the primary API surface.** The richest, most ergonomic layer got
   the most investment (13 modules, self-documenting registration, dual-return
   error idiom). The shell is the OS's UI, so the in-process coupling is a
   feature, not a bug.
3. **The lab API table** — chosen so native benchmark binaries could be
   *sterile*: no libc, no kernel linking, reproducible. The struct-of-pointers
   is the classic "stable ABI without a linker" move. Scope deliberately tiny;
   grew only twice (parallel-for, then the framebuffer for the raytracer).
4. **`int 0x80` works from ring 0** — the observation that unlocked hosted
   programs cheaply: the same trap gate serves ring-0 callers, so a
   syscall-shaped boundary cost one IDT entry and a dispatcher, no privilege
   machinery.
5. **Vendor a curated newlib, not port a libc.** `--disable-newlib-supplied-syscalls`
   + `MISSING_SYSCALL_NAMES` gives a full ANSI libc over ten bare-named stubs.
   Vendoring (vs. fetch-at-build) was chosen deliberately after the doomgeneric
   experience: the tree builds offline.
6. **Symbol-based program-model detection.** `elf64_symbol(image, "_start")`
   present ⇒ hosted, absent ⇒ lab (`lua_run.c:194`). No note sections, no
   magic — the crt0 symbol *is* the tag. Both models share the 0x400000 fixed
   link address and the same loader.
7. **Scalar-first syscall ABI.** Three register args, scalar returns, negative
   errno; struct-filling stays in libgloss (`fstat`, `gettimeofday`). Keeps
   the kernel ignorant of libc ABI — the cheapest possible FFI contract.
8. **Platform extensions grow as numbered syscalls** (fb → input → time →
   SFX → music streaming), each with a `juampi_*` C wrapper in `juampi.h`.
   The Doom port drove all of 10–20; each addition was demand-driven, none
   speculative.
9. **Whole-file FDs.** Hosted file I/O buffers the entire file in the kernel
   heap (matching the Lua `fs.*` model). Simple, correct, and fine at the
   4 MB-WAD scale; see §F6 for the limit.
10. **Two libcs, no sharing.** klibc for in-kernel Lua, newlib for hosted —
    compiled under different regimes (kernel flags vs. `-nostdinc`
    freestanding). The duplication (string functions, printf) was accepted to
    keep the coupling graph clean.

## Friction & improvements

> [!warning] Ordered by leverage; F1–F3 are cheap and worth doing soon.

### F1. The syscall table is duplicated by hand
`SYS_*` numbers live in **both** `src/syscall.c:28` and
`build/hosted/syscalls.c:15`, tied only by "must match" comments — now 21
numbers and growing (the audio arc added seven this week). One drifted define
would produce silent misbehavior, not an error.
**Fix:** one shared header of plain defines (e.g. `build/hosted/juampi_abi.h`)
included from both sides — it's pure preprocessor, so the kernel can include a
hosted-side header safely. Same file can carry the errno values and the
`ratevol`-style packing helpers.

### F2. Dead code: `elf64_load`
The ring-3 user-mapping loader (`src/elf64.c:52`) has **zero callers** — both
models load via `elf64_load_exec`. It's a leftover from a user-mode story that
never materialized. Delete it (git remembers), or keep it only if ring 3 is
actually on the roadmap; today it misleads readers into thinking a user-mode
path exists.

### F3. Build staleness on flag changes
Objects don't rebuild when their *flags* change (bit us this week: a stale
`i_sound.o` compiled without `-DFEATURE_SOUND` made the sound module silently
empty). **Fix:** a flags stamp file (write `$(HOSTED_CFLAGS)` etc. to
`obj/.flags`, depend objects on it) — standard make hygiene, one-time cost.

### F4. Hosted argv is vestigial
`run("doom.elf")` builds `argv = {name, NULL}` (`lua_run.c:195`) and drops any
further Lua arguments, while Doom parses `-episode`/`-skill`/`-nomusic` etc.
**Fix:** forward `run("doom.elf", "-nomusic")` varargs into argv — a ~20-line
change in `lua_run.c`, high demo value.

### F5. Lua error-convention drift
Most runtime conditions return `nil, err` (`fs.read`, `audio.play`,
`net.udp`), but a few throw instead — e.g. `fb.image` raises on a missing
file, `fb.canvas` raises when headless. Callers can't predict which without
reading the binding. **Fix:** normalize on *misuse throws / runtime returns
nil,err*, and note the rule in `luadoc.h` so new bindings inherit it.

### F6. Whole-file FDs meet big files
Fine today; the moment something wants streaming access to a file larger than
RAM headroom (music assets, a bigger WAD), `sys_open` buffering the whole file
becomes the limit. The ext2 layer already has `map_blocks`/contiguous-run
reads, so per-`read()` streaming is mostly plumbing. Not urgent — flag it as
the known ceiling.

### F7. The lab/hosted overlap question
Doom pulled the hosted model ahead: it now has files + graphics + input +
audio, while lab uniquely offers **SMP** and the **raw framebuffer**. The
models are one `parallel` syscall away from overlapping almost completely.
Options: (a) keep both, documented as niches (sterile benchmarks vs. real
programs) — the current de-facto state and a fine one; (b) add
`SYS_parallel` and let hosted programs (the raytracer) go multicore, then let
lab fade to benchmarks-only. Recommendation: (a) for now, (b) when a hosted
program actually needs cores — resist adding it speculatively.

### F8. Three-arg syscall ceiling
Already producing packing hacks (`ratevol` in `SYS_audio_play` packs rate and
volume into one register). Cheap fix when it next hurts: pass r10 as a fourth
arg in the trap stub, or adopt a pointer-to-struct convention for wide calls
(with the struct defined in the shared ABI header from F1).

## What's healthy (keep as-is)

- **luadoc registration** — self-documenting API surface; extend the idea, not
  just preserve it (the `__doc` tables could someday emit LuaLS stubs).
- **Scalar syscall ABI + libgloss struct-filling** — the kernel not knowing
  `struct stat` is exactly right.
- **Symbol-tag program detection** — zero-format-overhead dispatch that has
  survived three program types.
- **The capability-table lab ABI** — still the cleanest way to hand a
  freestanding binary a curated slice of the kernel.
- **Deliberate two-libc split** — the coupling graph (kernel ⊥ newlib,
  hosted ⊥ kernel headers) is the reason the Doom port stayed tractable.

## Related

- [[hosted-libc]] — the hosted model's own doc (build, limits, Doom).
- [[lua-shell]] — how the interpreter came to live in the kernel.
- [[reentrancy-audit]] — the mutable-state review that touches these layers.
