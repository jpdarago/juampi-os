---
title: Doom port
tags: [design, doom, hosted, audio, input, walkthrough]
status: complete
related: ["[[hosted-libc]]", "[[api-layers]]", "[[hda-audio]]", "[[Index]]"]
created: 2026-07-31
---

# Running Doom on juampiOS

> [!abstract] Goal
> A walkthrough of the Doom port: how the code is organized, the design
> decisions behind it, what changed in the kernel, which APIs the port
> introduced, and how the vendored engine differs from upstream.

## Overview

juampiOS runs Doom as a **hosted program**: an ordinary newlib-linked C
binary that executes in ring 0 and reaches the kernel only through the
`int 0x80` syscall boundary. The engine is
[doomgeneric](https://github.com/ozkl/doomgeneric), vendored under
`build/hosted/doom/`. The port added no Doom-specific code to the kernel.
Instead, Doom drove the growth of three general-purpose facilities:

- A **graphical platform layer** for hosted programs: framebuffer
  presentation, raw key events, and a millisecond clock.
- A **sound-effects path** into the existing kernel audio mixer.
- A **streaming-music path** that carries synthesized audio from a program
  to the mixer.

Every capability is reusable by any future hosted program.

## Terminology

| Term           | Meaning                                                                                                                                                                 |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Hosted program | A C binary built against the vendored newlib, entered via crt0 `_start`, run in ring 0, and served by the `int 0x80` dispatcher (`src/syscall.c`). See [[hosted-libc]]. |
| doomgeneric    | A Doom source port that isolates all platform work behind a small set of `DG_*` hooks and two sound-module vtables.                                                     |
| WAD            | Doom's data archive. The shareware `doom1.wad` lives on the ext2 data disk, not in the repository.                                                                      |
| DMX lump       | A sound-effect lump: header plus 8-bit unsigned mono PCM, padded with 16 samples at each end.                                                                           |
| MUS lump       | Doom's music format, a compact MIDI variant: channelized note events with variable-length delays at 140 ticks per second.                                               |
| GENMIDI lump   | A WAD lump that maps each General MIDI instrument to an OPL FM patch.                                                                                                   |
| OPL3           | The Yamaha FM-synthesis chip (YMF262) that produced DOS-era Doom music. The port emulates it with the vendored Nuked-OPL3.                                              |

## Architecture

```
 ┌──────────────────────────── doom.elf (ring 0, hosted) ────────────────────┐
 │ doomgeneric engine (vendored, GPLv2)                                      │
 │   ├── doomgeneric_juampi.c   video / input / timing (6 DG_* hooks)        │
 │   ├── i_juampisound.c        DG_sound_module: SFX -> mixer voices         │
 │   ├── i_juampimusic.c        DG_music_module: MUS -> OPL3 -> stream       │
 │   └── opl3.c                 Nuked-OPL3 emulator (vendored, LGPL 2.1)     │
 │ newlib (vendored subset) + libgloss stubs (syscalls.c) + juampi.h         │
 └────────────────────────────────┬──────────────────────────────────────────┘
                             int 0x80  (numbers: build/hosted/juampi_abi.h)
 ┌────────────────────────────────┴──────────────────────────────────────────┐
 │ kernel                                                                    │
 │   syscall.c dispatcher ── ext2 (WAD file I/O, NVMe/ATA)                   │
 │        │                ── gfx (fb_blit_centered, fullscreen handoff)     │
 │        │                ── keyboard (raw make/break ring)                 │
 │        └───────────────── audio.c mixer ── HDA / AC'97 backend (IRQ-fed)  │
 │                             ├── 16 PCM voices  (SFX)                      │
 │                             └── streaming ring (music)                    │
 └───────────────────────────────────────────────────────────────────────────┘
```

## How Doom starts and runs

1. `make doom` links `doom.elf` from the vendored engine, our three
   platform files, crt0, the libgloss stubs, and `libnewlib.a`, then packs
   it (with the WAD from `make doom-wad`) into the ext2 disk image.
2. In the shell, `run("doom.elf")` resolves the file from the data disk.
   The ELF exports `_start`, so the loader dispatches it to `hosted_run()`
   rather than the lab path (see [[api-layers]]).
3. `hosted_run()` loads the segments at 0x400000, allocates a 16 MiB sbrk
   heap, drains both keyboard rings, and calls `_start(argc, argv)`.
4. The engine opens `doom1.wad` through newlib `fopen`/`fread`, which the
   kernel serves as whole-file-buffered descriptors over ext2.
5. Each frame, `DG_DrawFrame` passes the 640×400 back buffer to
   `juampi_fb_present`. The first call suspends the desktop compositor and
   takes the screen; the kernel blits the frame centered.
6. `DG_GetKey` drains the raw keyboard ring: PS/2 set-1 make/break codes,
   translated to doomgeneric key codes, with press and release events.
7. Sound effects and music flow through the audio APIs described below.
   The mixer runs from the audio device's completion interrupt, so audio
   continues while the game loop monopolizes the CPU.
8. Quit Game calls `exit(0)`, which traps into the kernel and longjmps back
   to `hosted_run()`. Teardown closes descriptors, returns the screen to
   the desktop, drains the input rings, and resyncs the mouse.

## Design decisions

1. **Hosted model, not a kernel port.** The engine builds as a normal C
   program against newlib. The kernel gained *capabilities*, not Doom
   code. Consequence: every addition (graphics present, raw keys, audio
   play, streaming music) is a numbered syscall any program can use.
2. **Vendor the engine.** The first version fetched doomgeneric at build
   time out of GPL caution. Vendoring won: the tree builds offline, and
   the GPLv2/LGPL-2.1 obligations are satisfied by shipping the source
   (license notes in `build/hosted/doom/README.md` and the top-level
   README). Only the WAD remains a fetch (`make doom-wad`), because game
   data is not redistributable source.
3. **Present-a-frame graphics, not a drawing API.** Hosted programs get
   one graphics call: blit a complete frame. The kernel keeps ownership of
   modes, damage tracking, and the compositor handoff. Doom needs nothing
   more, and the API cannot be misused mid-frame.
4. **Reuse the kernel mixer for SFX.** The mixer already resampled,
   mixed, and ran from the completion IRQ. Doom's DMX effects are one-shot
   PCM clips, so each maps to a mixer voice. The alternative — a raw PCM
   device handed to the program — would have forfeited mixing with shell
   sounds and required rate conversion in the game.
5. **Generation-tagged voice handles.** Doom stops and queries sounds by
   channel long after a voice slot may have been recycled. A handle is
   `(generation << 8) | slot`, so a stale handle cannot stop or query a
   newer sound. This hardened the mixer for all callers.
6. **A streaming ring for music, not a giant PCM voice.** Music is
   open-ended and synthesized incrementally, so it cannot be a finite
   voice buffer. The kernel added a lock-free single-producer,
   single-consumer ring (32,768 stereo frames ≈ 0.68 s) that the mixer
   drains one frame per output frame. Underrun contributes silence, never
   a click.
7. **OPL FM synthesis for music, in the program.** Authentic DOS-Doom
   sound with zero external assets: the GENMIDI patches come from the WAD,
   and the MUS lumps are parsed directly (the event grammar mirrors the
   vendored `mus2mid.c`). The synth runs in the *program*, not the kernel
   — the kernel only transports finished 48 kHz stereo samples. Nuked-OPL3
   was vendored rather than hand-rolling FM: it is cycle-accurate,
   dependency-free (no libm; its sine table is baked in), and resamples to
   48 kHz internally.
8. **Timing by fractional carry.** The MUS clock is 140 Hz; the mixer is
   48 kHz. The music module converts ticks to samples with a running
   remainder, so long songs do not drift. The module's `Poll` hook (called
   about 35 times per second by the engine) renders up to each event batch
   and tops up the ring.
9. **Perceptual volume mapping.** The OPL total-level register is
   logarithmic. Mapping linear MIDI volume onto it lands mid volumes about
   20 dB too quiet, so the port uses chocolate-doom's perceptual
   volume-mapping table.

## Kernel changes

| Subsystem | File(s) | Change |
|-----------|---------|--------|
| Syscall dispatcher | `src/syscall.c` | Platform syscalls 10–20 (see the API section). Entry/exit now drain both keyboard rings; exit also resyncs the mouse and returns the screen to the compositor. |
| Audio mixer | `src/audio.c`, `include/audio.h` | Voice handles are generation-tagged; `audio_voice_active()` added; `MAX_VOICES` raised 8 → 16; per-voice stop now fades ~2.7 ms instead of cutting; streaming-music ring added (`audio_music_start/write/space/stop`). |
| SFX conversion | `src/syscall.c` | `sys_audio_play` converts 8-bit unsigned PCM to s16 and applies a ~2 ms declick ramp at both ends (DMX samples rarely rest at the DC midpoint). |
| ATA driver | `src/ata.c` | PIO transfers use `rep insw`/`rep outsw` — one string instruction per sector instead of 256 port reads, each a VM exit under KVM. |
| ext2 reader | `src/ext2.c` | `map_blocks()` resolves the full block map reading each indirect block once; `read_file` reads physically contiguous runs in one block-device call. |
| Keyboard driver | `src/keyboard.c` | The IRQ handler checks the i8042 status register and routes pending aux (mouse) bytes to the mouse parser instead of consuming them as scancodes. |
| Mouse driver | `src/mouse.c` | Packet assembly factored into `mouse_handle_byte()` (callable from the keyboard IRQ); `mouse_flush()` resets packet phase and drops stale motion. |
| Build | `Makefile` | `make doom`, `make doom-wad`, `make doom-src`; the data disk attaches as NVMe in `make run`; flag-stamp files and hosted `-MMD` dep tracking (see [[api-layers]] F3). |

The three input/audio defects fixed after the first playthroughs — phantom
keys at startup, clicks when voices stop, and a dead mouse after exit —
are documented in the commit `9fda471` and summarized under
[Defects found during integration](#defects-found-during-integration).

## New APIs

### Syscalls (build/hosted/juampi_abi.h)

The numbers live in one shared header included by both the kernel
dispatcher and the libgloss stubs. All calls use the existing convention:
number in `rax`, arguments in `rdi/rsi/rdx`, result in `rax`.

| Number | Name | Purpose |
|--------|------|---------|
| 10 | `SYS_fb_info` | Screen size as `(height << 16) \| width`, or −1 when headless. |
| 11 | `SYS_fb_present` | Blit a w×h `0x00RRGGBB` frame, centered. First call takes the screen from the desktop. |
| 12 | `SYS_getkey` | Next raw key event: PS/2 set-1 make code (bit 7 = E0-extended) plus a pressed/released flag. |
| 13 | `SYS_ticks_ms` | Milliseconds since boot (monotonic). |
| 14 | `SYS_audio_play` | Play 8-bit unsigned mono PCM; returns a voice handle. `rdx` packs volume and sample rate. |
| 15 | `SYS_audio_stop` | Stop (fade out) one voice handle. |
| 16 | `SYS_audio_active` | Whether a voice handle is still audible. |
| 17 | `SYS_audio_music_start` | Enable the streaming-music ring and clear it. |
| 18 | `SYS_audio_music_write` | Enqueue 48 kHz stereo s16 frames; returns the count accepted. |
| 19 | `SYS_audio_music_space` | Free frames in the ring, for producer pacing. |
| 20 | `SYS_audio_music_stop` | Disable streaming and drop buffered audio. |

### C wrappers (build/hosted/juampi.h)

Hosted programs call the syscalls through thin wrappers:
`juampi_fb_info`, `juampi_fb_present`, `juampi_getkey`,
`juampi_ticks_ms`, `juampi_audio_play/stop/playing`, and
`juampi_music_start/write/space/stop`. The header also defines `JK_*`
constants for common make codes.

### Kernel mixer additions (include/audio.h)

`audio_voice_active(voice)` reports whether a handle still plays.
`audio_music_start/write/space/stop` expose the streaming ring inside the
kernel; today the syscalls are their only caller, but the API is
kernel-general (a Lua binding would be a small addition).

## Changes to the vendored engine

The engine is upstream doomgeneric with the other platform frontends and
the SDL/Allegro sound backends pruned (`build/hosted/fetch-doom.sh`
re-fetches and re-prunes). The port touches upstream files as little as
possible:

| File | Status | Change and reason |
|------|--------|-------------------|
| `doomgeneric_juampi.c` | **Ours** | The platform frontend: six `DG_*` hooks for video (present via `juampi_fb_present`, auto-scaled 2× to 640×400), input (raw make/break → doomgeneric key codes), and timing. |
| `i_juampisound.c` | **Ours** | `DG_sound_module`: maps each logical sound channel to a mixer voice; parses DMX lumps (strips the 16-sample padding); provides the `use_libsamplerate`/`libsamplerate_scale` config variables that the pruned SDL backend used to define. |
| `i_juampimusic.c` | **Ours** | `DG_music_module`: parses MUS events, applies GENMIDI patches to nine OPL voices (OPL3 stereo mode), renders through Nuked-OPL3, and streams to the kernel ring. |
| `opl3.c`, `opl3.h` | **Vendored** (Nuked-OPL3, LGPL 2.1) | Cycle-accurate OPL3 emulator; unmodified. |
| `i_system.c` | **Patched** | `I_Quit` calls `exit(0)` unconditionally. Upstream guarded the call behind `#if ORIGCODE`, so Quit Game fell back into the game loop instead of returning to the shell. |
| `i_sound.c` | **Patched** | Removed the `#include <SDL_mixer.h>` that `FEATURE_SOUND` pulled in. The include was vestigial — sound modules are ours — but it broke the build with no SDL present. |
| Everything else | **Verbatim** | Unmodified upstream source (GPLv2). |

The engine builds with `-DFEATURE_SOUND`, which activates the two
`DG_*_module` vtables in `i_sound.c`.

## Performance: 80 s startup to ~4 s

The first boot took about 80 seconds to load the 4 MB WAD. Three changes
fixed it:

1. ATA PIO moved each sector as 256 individual `inw` instructions — every
   one a VM exit under KVM, roughly two million exits for the WAD. String
   I/O (`rep insw`) reduces that to one exit per sector.
2. The ext2 reader resolved the block map per data block, re-reading the
   same indirect blocks repeatedly. `map_blocks()` reads each indirect
   block once and `read_file` fetches contiguous runs in single device
   calls.
3. `make run` attaches the data disk as NVMe (DMA, MSI-X) instead of IDE,
   matching the real-hardware target. The smoke tests still attach IDE to
   keep `ata.c` covered.

## Defects found during integration

Each of these shipped, was reported from play-testing, and is fixed:

- **Phantom input at startup.** The raw key ring accumulates every
  make/break since boot, and nothing drained it before a program started
  — Doom replayed the shell's typing history and opened menus on its own.
  A second source: the keyboard IRQ handler consumed pending *mouse* bytes
  as scancodes (the i8042 output buffer is shared), injecting ghost keys
  whenever the mouse moved. Fixes: drain the rings on program entry;
  route aux bytes by status bit.
- **Dead mouse after exit.** The same swallowed aux bytes desynced the
  3-byte mouse packet stream, and delta bytes with bit 3 set parse as
  packet headers, so the desync persisted. Fixes: the routing above, plus
  a packet-phase reset on program exit.
- **Clicks at the end of sound effects.** Two causes, fixed in layers:
  DMX samples do not end at the DC midpoint (fixed with a ~2 ms edge ramp
  at conversion), and `audio_stop` cut voices mid-waveform on Doom's
  constant channel reuse (fixed with a ~2.7 ms mixer fade on stop).
- **Silent sound modules from a stale object.** Enabling `FEATURE_SOUND`
  did not rebuild `i_sound.o`, because make does not track flag changes;
  the stale object had an empty module list. This motivated the build
  system's flag stamps (see [[api-layers]] F3).

## Build and run reference

```
make doom-wad    # fetch doom1.wad onto the ext2 disk staging dir (once)
make doom        # build doom.elf and rebuild disk.img
make run         # boot QEMU with the data disk on NVMe and AC'97 audio
run("doom.elf")  -- in the shell
```

Headless verification uses `-audiodev wav` capture analyzed with sox, and
QMP `send-key`/`screendump` to drive and observe the game. See the test
recipes in the QEMU-harness memory note and `tests/`.

## Limitations and future work

- Stereo panning is not implemented; SFX play centered (`sep` ignored).
- Music timbre is verified by capture statistics, not by ear, against the
  OPL reference; instrument-level comparison remains open.
- `run("doom.elf", ...)` does not forward extra arguments to `argv`, so
  engine flags like `-nomusic` are unreachable ([[api-layers]] F4).
- Saving games writes through the whole-file descriptor path; large-file
  streaming I/O remains the known ceiling ([[api-layers]] F6).
