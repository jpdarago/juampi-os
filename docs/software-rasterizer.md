---
title: Software rasterizer & graphics roadmap
tags: [design, graphics, rasterizer, fonts, ui, hosted, roadmap]
status: planned
related: ["[[hosted-libc]]", "[[doom-port]]", "[[ui]]", "[[api-layers]]", "[[Index]]"]
created: 2026-08-05
---

# Software rasterizer & graphics roadmap

> [!abstract] Goal
> juampiOS has no GPU and never will on the real target (the XPS's Intel
> graphics is an i915-class monster). All rendering is CPU pixel-pushing.
> Today each subsystem — the compositor, `microui`, `flanterm`, Doom, the
> hosted `fb_present` blit — rolls its own subset. This note plans a **shared
> software 2D rasterizer** as the keystone, the features that sit on top of
> it, and the order to build them. Keeping the roadmap here so we don't lose
> track of the pieces.

## Where we are now

The current graphics stack, bottom to top:

- **Framebuffer** — Limine hands us a linear framebuffer; `src/gfx.c` knows its
  base, pitch, and per-channel shifts (`gfx_shifts()`), and does the pixel
  repack from our internal `0x00RRGGBB` to the hardware layout.
- **Primitives** — ad-hoc pixel/line/rect/blit code, duplicated across `gfx.c`,
  the compositor, and Doom.
- **Compositor** — damage-flip compositing for the desktop;
  `ui_fullscreen_begin()/end()` suspends it for fullscreen apps.
- **UI** — `microui` immediate-mode UI (`src/ui.c`) + `flanterm` terminal.
- **Assets** — QOI image decode (`src/qoi.c`), `font8x16` bitmap glyphs.
- **Hosted apps** — reach the screen only through `SYS_fb_info` / `SYS_fb_present`
  (a synchronous, centered, per-pixel repacking blit of a `0x00RRGGBB` buffer).
  See [[hosted-libc]] and [[doom-port]].

> [!note] The gap
> There is no reusable rasterizer and no scalable text. Doom, the UI, and
> hosted blits each reinvent a slice. Fixing that is the multiplier below.

## The keystone: a shared software 2D rasterizer

A single `raster` library — used by the compositor, the UI, hosted programs,
and any future [[#raylib|raylib]] backend — instead of N ad-hoc copies.

Surface (roughly, grown as needed):

- A **surface** abstraction: `{pixels (0x00RRGGBB or 0xAARRGGBB), w, h, pitch}`,
  so the framebuffer, an offscreen backbuffer, and a texture are the same type.
- **Filled + stroked polygons**, triangles, rects, rounded rects, circles.
- **Textured triangles / quads** (affine for 2D; the 3D path adds perspective).
- **Alpha blending** + a small set of blend modes (over, add, multiply).
- **2D transforms** (a 3×3 or 2×3 matrix stack): translate/scale/rotate.
- **Clipping** to a rect (and ideally an arbitrary scissor).
- **Anti-aliasing** via coverage (Wu lines, AA polygon edges).
- **Blit** with scale/rotation and per-pixel alpha (glyphs, sprites, cursors).

Why it is the keystone: the text rasterizer, the windowing upgrade, a Lua
canvas, and the raylib port are all *thin* on top of this, and *fat* without it.

> [!tip] Design bias
> Match the existing software-framebuffer ethos (Doom, `gfx.c`). Keep the
> internal pixel format `0x00RRGGBB` and repack once at present time, so the
> rasterizer never touches hardware layout.

## Tracks on top of the rasterizer

### Text: scalable, anti-aliased fonts

Replace `font8x16` with a **TrueType rasterizer** (vendored `stb_truetype`):
crisp AA text at any size, kerning, a glyph cache. Upgrades the shell, editor,
and UI immediately. *This is where we're starting (see below).* — **medium
effort, high visible payoff.**

### Windowing / desktop

Grow the damage-flip compositor into real windows: z-order, drag, decorations,
alpha-blended surfaces, drop shadows, a taskbar, and a **software mouse
cursor**. Add a proper double-buffered swapchain for hosted programs (today
`fb_present` is a synchronous centered blit with no app-owned backbuffer). See
[[ui]]. — **large, but very "OS," very demoable.**

### General software 3D engine

A `tinyrenderer`-style engine: z-buffer, perspective-correct texturing,
flat/Gouraud shading, matrix pipeline, OBJ loader. Fun, bounded, and it is the
fixed-function path that would give raylib cheap 3D. — **medium-large.**

### Image formats

Have QOI decode; add `stb_image` (PNG/JPEG/BMP) for real assets, and QOI/PNG
*encode* for screenshots. — **medium.**

### Lua creative-coding canvas

Grow the existing Lua framebuffer binding (`src/lua/lua_fb.c`) + the
`raytracer.lua` precedent into a real 2D canvas API (shapes, images, text,
transforms, present, input) so the shell becomes a demoscene / plotting /
generative-art playground. — **small-medium, best fun-to-effort ratio.**

## Quick wins

- **Screenshots**: capture framebuffer → QOI/PNG on disk. ~a day.
- **Expose mouse to hosted programs** (`SYS_getmouse`): kernel already tracks
  the mouse; there is no syscall. Unblocks interactive hosted apps + raylib.
  *(In progress — see below.)*
- **Software mouse cursor** overlay in the compositor.
- **AA primitives**: Wu lines, coverage-AA circles/polys — a quiet quality jump.

## Driver track (QEMU-only)

Mode setting / `virtio-gpu` 2D: resolution control, a hardware cursor plane,
page-flipping (tear-free). Real driver work, scoped in QEMU — the XPS's Intel
GPU is out of reach, so this track is emulator-only. — **medium.**

## raylib connection

raylib has **no software renderer**: every draw call funnels through `rlgl`
into an OpenGL context. Porting it is really *"supply a GL-ish backend for a
framebuffer OS,"* and ~80% of the rest (libc/libm, timing, input, audio
streaming, ext2 files, `fb_present`) already exists. The **2D rasterizer above
is exactly the missing renderer** — a minimal software `rlgl` backend (colored
+ textured triangles, alpha blend, ortho matrix, `RenderTexture` = another
target surface) makes a 2D raylib port a ~1–2 week job on top of it. 3D/shaders
are a separate, much larger effort. See the raylib scoping discussion; this doc
is the substrate that unblocks it.

## Recommended sequence

1. **Quick wins first for momentum**: mouse syscall, hosted heap bump,
   `fb_present` fast path, screenshots.
2. **Text**: vendor `stb_truetype` + a glyph-cached font rasterizer.
3. **The keystone**: extract the shared 2D rasterizer (surfaces, transforms,
   AA polygons, textured blit, blend).
4. **Windowing** upgrade on top of it (+ software cursor, app backbuffers).
5. Then pick a showpiece: **3D engine**, **Lua canvas**, or the **raylib** port.

## Status / work log

> [!note] In progress (2026-08-05)
> Kicking off the quick wins + the font track, ahead of extracting the shared
> rasterizer:
> - [ ] `SYS_getmouse` — expose the mouse to hosted programs.
> - [ ] Bump the hosted `sbrk` heap (16 MiB → larger) for asset-heavy programs.
> - [ ] `fb_present` fast path when the native format already matches
>       `0x00RRGGBB` (row `memcpy` instead of per-pixel repack).
> - [ ] Vendor `stb_truetype`.
> - [ ] Font rasterizer (scalable AA glyphs + cache) on top of it.
