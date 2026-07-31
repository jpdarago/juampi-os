# Hosted C programs (newlib) in ring 0

juampiOS can run ordinary ANSI-C programs — `main(argc, argv)`, stdio, `malloc`,
math, file I/O — with few or no source changes, by linking them against
**newlib** and running them in ring 0 (the same "lab" execution model as the
native benchmark ELFs). They reach kernel services through an `int 0x80` trap
rather than by linking against kernel symbols, so the program's libc is fully
self-contained.

This is the "path B" libc: **newlib**, not musl (musl assumes the Linux syscall
ABI and is hard to retarget); newlib is built for retargeting via a thin syscall
stub layer. See the discussion in the project history for the musl-vs-newlib
reasoning.

## Pieces

- **newlib** — a **curated subset vendored under `src/newlib/`** and compiled
  per-file by the host GCC in the Makefile (like BearSSL/Lua/uACPI). The subset
  (63 libc `.c` files: stdio, stdlib incl. `dtoa`/`mprec`, string, reent, ctype,
  locale, signal, errno) was chosen from a linker map of the demos, so nothing
  unused is carried. Subdirs are preserved so same-dir `"local.h"` includes
  resolve; the installed public headers sit in `src/newlib/include`. Compile
  flags that matter: `-D_LIBC` (disable newlib's fortify/ssp wrappers — the nix
  gcc wrapper's default `-D_FORTIFY_SOURCE=2` would otherwise pull them in),
  `-DMISSING_SYSCALL_NAMES` (reent layer calls our bare-named syscalls),
  `-DHAVE_MMAP=0` (sbrk-only malloc), `-nostdinc` + newlib headers only.
  Re-vendoring pipeline (only needed to change the subset): `build-newlib.sh`
  does newlib's autotools build for `x86_64-elf` (via the nix
  `pkgsCross.x86_64-embedded` toolchain) into the gitignored `.newlib/`, then
  `vendor-newlib.sh` copies the curated files into `src/newlib/`.
- **crt0** (`build/hosted/crt0.S`) — `_start(argc, argv)`: run `.init_array`
  constructors, call `main`, hand the result to `exit()` (which flushes stdio and
  traps out via the exit syscall). Also supplies empty `_init`/`_fini`.
- **libgloss stubs** (`build/hosted/syscalls.c`) — the bare POSIX hooks newlib's
  reentrant layer expects (`write`, `read`, `open`, `close`, `lseek`, `sbrk`,
  `fstat`, `isatty`, `gettimeofday`, `getpid`/`kill`/…). Each issues `int 0x80`
  (number in rax, args rdi/rsi/rdx, return in rax; negative = -errno). `fstat`
  and `isatty` are resolved locally so the kernel needn't know newlib's
  `struct stat` layout.
- **kernel syscall dispatcher** (`src/syscall.c`) — implements the trap over
  console I/O, a per-program `sbrk` heap, the RTC clock, and an ext2-backed file
  descriptor table. `int 0x80` works from ring 0 (a software interrupt through
  the DPL-3 gate); no privilege switch happens, but it's still a clean, stable
  boundary that would carry over to a future ring-3 port.
- **runner** — `hosted_run()` (in `src/syscall.c`) loads the ELF, gives it a
  heap + argv, jumps to `_start`, and returns `main`'s status. `exit` longjmps
  back (then re-enables interrupts — the int gate masked them, as with fault
  recovery). `run("prog.elf")` in `lua_run.c` routes here when the ELF defines
  `_start` (a hosted program); lab ELFs (entry `bench`) take the existing path.
  `elf64_symbol()` makes that distinction.

## Execution model

One hosted program runs at a time, synchronously, on the caller's stack, in ring
0 with no memory isolation (NX/SMEP/SMAP are off). A fault in the program is
caught by the shell's fault recovery, like any other ring-0 code. This is a
scriptable "run a C program" facility, not a protected multiprocess userland —
that would want the (currently dormant) ring-3 path plus preemptive scheduling.

Files are buffered whole in the kernel heap: reads serve from the buffer, writes
grow it, and `close` (or program teardown) flushes a dirty file to ext2 with
`ext2_write_file` — matching the raw-block Lua API's simple whole-file model.

## Building and running

Just `make` — the vendored newlib compiles with the host GCC, no cross toolchain
needed. The demo programs `build/hosted/chello.c` (printf/snprintf/malloc) and
`filetest.c` (fopen/fread/fwrite over ext2) build into Limine modules; run them
from the shell with `run("chello.elf")` / `run("filetest.elf")`. Both are in
`make test` (`HOSTED_OK`, `FILEIO_OK`).

To add a program: drop a `.c` in `build/hosted/`, add its name to `HOSTED_PROGS`
in the Makefile, and add a `module_path` line to `build/limine.conf`.

## Graphical programs + Doom

Beyond POSIX, hosted programs get a small platform layer (`build/hosted/juampi.h`,
served by `src/syscall.c`) for interactive graphics:

- `juampi_fb_present(pixels, w, h)` — blit a `w*h` `0x00RRGGBB` buffer, centered
  and channel-packed, to the framebuffer. The first call takes the screen from
  the desktop compositor (`ui_fullscreen_begin`); it's handed back on exit, so
  text programs stay windowed and only graphical ones go fullscreen.
- `juampi_getkey(&pressed)` — a raw key event (PS/2 make code, bit 0x80 =
  E0-extended) with press **and** release, from a second keyboard ring
  (`keyboard_poll_raw`) that the ASCII line-editing ring doesn't expose.
- `juampi_ticks_ms()` — a millisecond clock; `juampi_fb_info()` — screen size.

`build/hosted/gfxdemo.c` (a bouncing ball) is the minimal example.

**Doom** runs on this: the doomgeneric engine is vendored (GPLv2, committed under
`build/hosted/doom/`; `make doom-src` re-fetches it), and `make doom` fetches the
shareware WAD (`make doom-wad`, onto the ext2 disk), links `doom.elf` onto the
disk, and rebuilds the ext2 image. Boot with the data disk attached, then
`run("doom.elf")` — it loads `/doom1.wad`, renders 320x200 auto-scaled 2x via
`fb_present`, and takes input via the raw key events. Our frontend implements
doomgeneric's 6 hooks and stubs the POSIX bits Doom references but doesn't need.

Doom has **sound effects**: built with `-DFEATURE_SOUND`, our backend
`build/hosted/doom/i_juampisound.c` implements doomgeneric's `DG_sound_module`.
Each logical channel maps to a kernel mixer voice: `StartSound` parses the DMX
lump (8-bit unsigned mono PCM, 16-sample pad stripped each end) and hands it to
`juampi_audio_play()` (a new syscall), which the kernel resamples (11 kHz →
48 kHz) and mixes; the AC'97/HDA completion IRQ keeps it fed while Doom runs its
own loop. Stop/IsPlaying use **generation-tagged** voice handles (`gen<<8 | slot`)
so a recycled slot can't be mistaken for the original play. `sys_audio_play`
applies a ~2 ms declick ramp to each one-shot, since DMX samples rarely rest at
the DC midpoint and would otherwise snap to silence with a click. To enable
`FEATURE_SOUND` we dropped i_sound.c's unused `<SDL_mixer.h>` include and defined
the two `use_libsamplerate`/`libsamplerate_scale` config knobs the (unbuilt) SDL
backend would have provided.

Doom also has **OPL FM music** — the authentic DOS-Doom sound, no external
assets. `build/hosted/doom/i_juampimusic.c` implements `DG_music_module` over a
vendored cycle-accurate OPL3 emulator (`opl3.c`, Nuked-OPL3, LGPL 2.1): it parses
the MUS lump directly, maps each note to an FM patch from the WAD's `GENMIDI`
lump, drives the emulated OPL (9 voices, OPL3 mode for stereo), renders 48 kHz
stereo, and pushes it to a new **kernel streaming-music path**. That path
(`audio_music_*` in src/audio.c, `juampi_music_*` syscalls) is a lock-free
single-producer/single-consumer ring the mixer drains one frame per output frame,
mixed alongside the SFX voices — an open-ended source a software synth keeps
topped up. The song clock runs at the 140 Hz MUS tick rate; the module's `Poll`
hook (called ~35 Hz by the game) renders up to the next event, processes that
event batch, and repeats, converting ticks to samples with a fractional carry so
timing doesn't drift. A perceptual volume curve maps MIDI velocity/volume onto
the OPL's logarithmic level register.

It starts in a few seconds: `make run` attaches the
data disk as **NVMe** (DMA + MSI-X completions — the ext2 mount prefers NVMe over
ATA), and the ext2 reader loads the WAD in batched contiguous runs. (The earlier
~80 s came from ATA PIO transferring one 16-bit word per `inw()` — a VM exit each
— plus re-reading indirect blocks per data block; both are fixed.)

## Known limits / follow-ups

- This newlib's `printf` mishandles the **`%z`** length modifier — use `%u`/`%lu`.
- The vendored subset covers stdio/stdlib/string/math-printf; a program using
  more of libc (e.g. `<math.h>` functions, `qsort`, more of `<time.h>`) will hit
  an undefined reference — add the needed `.c` files to `src/newlib/` (re-run
  `vendor-newlib.sh` with the extra members) and rebuild.
- No real process model: `fork`/`exec`/signals are `ENOSYS` stubs; `getpid` is 1.
- Stdout is line-buffered via the console; there is no `stdin` line editing beyond
  the console's own.
