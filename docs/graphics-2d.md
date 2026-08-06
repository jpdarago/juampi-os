---
title: 2D graphics library (software rasterizer)
tags: [design, graphics, rasterizer, gfx, proposal]
status: design
related: ["[[software-rasterizer]]", "[[ui]]", "[[doom-port]]", "[[api-layers]]", "[[Index]]"]
created: 2026-08-06
---

# 2D graphics library (software rasterizer)

> [!abstract] Goal
> Turn today's scattered pixel-pushing into one shared, coherent software 2D
> rasterizer — the keystone of the graphics roadmap ([[software-rasterizer]]).
> Everything stays CPU-only (no GPU, ever, on the real target). This note is the
> design: what exists, the model to grow it into, the API, and the phased
> migration. It proposes decisions and flags the ones worth arguing about.

## Current state (what we're evolving, not replacing)

The seed is already here: `struct gfx_surface` (`gfx.h`) — a pixel buffer with
geometry, per-channel shifts, and an exclusive clip box — is the universal
drawing target for the screen, the double-buffer, and every `ui.canvas`. Keep
it. But the layer around it is inconsistent and duplicated:

- **Clipping is applied unevenly.** `gfx_fill`/`gfx_glyph`/`gfx_image` honor the
  clip box; `gfx_pixel`/`gfx_rect`/`gfx_line` honor only the surface bounds
  (`gfx.c`). So a clipped `fb.line` in a `ui.canvas` can scribble outside it.
  This is a latent bug, not a feature.
- **There is no real blend.** Every primitive overwrites. The *one* true
  alpha-blend in the tree is `ttf.c:blend_pixel` (unpack dst via shifts →
  `dst + (src-dst)*a/255` → repack). `gfx_blit` does only binary alpha-*keying*
  (`a==0` skip, else opaque). Nothing does blend modes, partial alpha, or
  coverage outside text.
- **Channel packing is copy-pasted four times.** `gfx.c:surf_pack`,
  `syscall.c:fb_blit_centered`, `doom/i_video.c:cmap_to_fb`, and
  `lab/raytracer.c:render_worker` each re-implement `(r<<rs)|(g<<gs)|(b<<bs)`.
- **No transforms, no anti-aliasing (except text), no textured triangles.**
  These are exactly what a nicer UI, vector graphics, and a raylib backend need.

Consumers split cleanly: `microui`/`ui.c` and the Lua `fb.*` binding already go
through `gfx_*`; Doom, `fb_present`, and the raytracer each roll their own loop.
The library's job is to make `gfx_*` complete and correct enough that the
hand-rolled loops fold back into it.

## Design principles

1. **Evolve `gfx_surface`; don't fork it.** It is already the shared currency.
   The library is "finish the drawing layer over it," not a parallel API.
2. **Internal color is `0xAARRGGBB`; repack once, at the edge.** The rasterizer
   works in a fixed layout; only `surf_pack`/`surf_unpack` touch the hardware
   shifts. This kills the four duplicated packers.
3. **Blend is first-class but has a fast opaque path.** Alpha `0xFF` (and legacy
   `0xRRGGBB`, read as opaque) takes a straight write; alpha `< 0xFF` blends.
   One model, no cost when you don't use it.
4. **Allocator-injected, single-threaded like the rest of graphics.** Surfaces
   and scratch come from a passed allocator; the drawing path is BSP-only for
   now (same as the compositor and `fb.*`). Reentrancy is a later, separate step.
5. **Two layers.** A fast **primitive layer** (axis-aligned fills, blits, text —
   what UI needs) and a general **rasterizer layer** (transformed, textured,
   anti-aliased triangles — what raylib/vector graphics need). 2D shapes can use
   either; the primitive layer is the fast common case.

## The model

### Color and the pack/unpack core

```c
// 0xAARRGGBB. Opaque legacy 0xRRGGBB works unchanged (alpha 0 is treated as
// 0xFF on input by the primitive layer, so existing callers keep overwriting).
typedef uint32_t gfx_rgba;

// The ONLY code that knows the hardware channel layout. Everything else is
// 0xAARRGGBB. Replaces surf_pack + the three copies elsewhere.
static inline uint32_t surf_pack(const struct gfx_surface* s, uint32_t argb);
static inline uint32_t surf_unpack(const struct gfx_surface* s, uint32_t native);
```

`gfx_blend` — the pixel op, generalizing `ttf.c:blend_pixel`:

```c
enum gfx_blend {
    GFX_COPY,      // src replaces dst (the current overwrite behavior)
    GFX_OVER,      // src-over-dst by src alpha (the ttf blend, generalized)
    GFX_ADD,       // additive (glows, particles)
    GFX_MUL,       // modulate (shadows, tints)
};

// One clipped, blended pixel. coverage in [0,255] scales src alpha (this is how
// AA edges and glyph coverage feed the same path). Honors s->clip + bounds.
static inline void gfx_plot(struct gfx_surface* s, int x, int y,
                            uint32_t argb, unsigned coverage, enum gfx_blend);
```

Every primitive bottoms out in `gfx_plot` (or a span variant for fills/blits).
`ttf_draw` stops carrying its own `blend_pixel` and calls `gfx_plot(..., cov,
GFX_OVER)`.

### The surface stays, with a small addition

`struct gfx_surface` is unchanged except it *documents* that `pixels` is
`0xAARRGGBB` for offscreen surfaces and native for the screen (the shifts
distinguish them). Add helpers so nobody hand-builds one again:

```c
struct gfx_surface gfx_surface_over(uint32_t* px, int w, int h); // 0xAARRGGBB, tight
// (gfx_surface_make already does native-layout; keep it.)
```

## Primitive layer (finish + fix `gfx_*`)

Same names, three changes: **every** primitive respects the clip box; each grows
an optional blend (defaulting to `GFX_COPY` so callers are unaffected); the blit
family is unified.

```c
void gfx_pixel (surf, x, y, argb);                     // now clip-correct
void gfx_fill  (surf, x, y, w, h, argb);               // (already clip-correct)
void gfx_line  (surf, x0,y0,x1,y1, argb);              // now clip-correct
void gfx_rect_outline(surf, x, y, w, h, argb);
// Blended variants (or a trailing enum gfx_blend arg — see Open decisions):
void gfx_fill_blend(surf, x, y, w, h, argb, enum gfx_blend);

// One blit to rule them: src is a gfx_surface (0xAARRGGBB or native), copied to
// dst at (x,y), clipped, with a blend. Subsumes gfx_image (native copy),
// gfx_blit (alpha-key), and fb_blit_centered (centered repack).
void gfx_blit(struct gfx_surface* dst, int x, int y,
              const struct gfx_surface* src, enum gfx_blend);
```

This alone: fixes the clip bug, gives the UI real translucency (menus,
shadows), and lets `fb_present`/Doom/`gfx_image` share one blit.

## Rasterizer layer (new — the raylib/vector core)

### Transform stack

A 2×3 affine matrix stack, used by the transformed blit and the triangle
rasterizer (not by the axis-aligned fast primitives).

```c
struct gfx_xform { float a, b, c, d, e, f; }; // [x' y'] = M * [x y 1]
void gfx_push(struct gfx_surface*, const struct gfx_xform*); // compose
void gfx_pop(struct gfx_surface*);
// translate / scale / rotate helpers compose onto the top.
```

### Textured / colored triangle — the keystone

The single primitive a software `rlgl` needs; 2D shapes, sprites, scaled/rotated
images, and gradients all decompose into it.

```c
struct gfx_vertex { float x, y, u, v; uint32_t argb; };
// Rasterize one triangle into dst under the current transform: per-pixel it
// interpolates color and (if tex != NULL) samples tex at (u,v), multiplies, and
// blends. Affine interpolation (2D — no perspective divide). Clipped + AA edges.
void gfx_triangle(struct gfx_surface* dst, const struct gfx_vertex v[3],
                  const struct gfx_surface* tex, enum gfx_blend);
```

With this, a scaled/rotated sprite is two textured triangles; a gradient rect is
two colored triangles; `RenderTexture` is just another `gfx_surface` as the
target. This is the whole basis of a software raylib backend (see
[[software-rasterizer#raylib connection]]).

### Anti-aliasing

Coverage-based, reusing `gfx_plot`'s `coverage` argument:
- **Lines:** Wu (fractional endpoint + edge coverage).
- **Polygon/triangle edges:** analytic edge coverage (distance-to-edge → alpha),
  or 4× supersample as a first, simpler cut.
- Text already works this way; the machinery is shared.

### Numerics

Edge functions and interpolation in **`float`** initially (the raytracer and
`ttf` already use FP freely; clarity first). Fixed-point (e.g. 28.4) is a later,
localized optimization if a profile says the triangle path is hot.

## Migration & phasing

Each phase builds, ships, and is independently useful. Order chosen so the
riskiest new code (triangles) lands last, on top of a de-duplicated base.

1. **Unify packing + fix clipping.** Extract `surf_pack`/`surf_unpack`; route
   `fb_blit_centered`, Doom's `cmap_to_fb`, and the raytracer through them; make
   `gfx_pixel`/`rect`/`line` clip-correct. *Pure cleanup + a bug fix; no API
   change.* Regression = existing UI/Doom/boing smoke tests.
2. **Blend pipeline.** Add `gfx_plot` + `enum gfx_blend`; move `ttf_draw` onto
   it; add blended fill + the unified `gfx_blit`; give the UI translucent
   popups/shadows as the proof.
3. **Transform stack + textured triangle + AA.** The rasterizer layer. Proof: a
   Lua/`fb` demo drawing a rotated textured quad and an AA polygon; a screendump
   regression test (the marker tests can't see pixels — see the ttfdemo bug).
4. **Software `rlgl` shim (separate note).** Map `rlBegin/rlVertex/rlColor/
   rlTexCoord` onto `gfx_triangle` + the transform stack — the raylib port.

Consumers to retire onto the shared layer as phases land: `fb_present`,
Doom present, `gfx_image`/`gfx_blit`, `ttf` blend, and eventually the boot-logo
and canvas paths.

## What it unblocks

- **UI:** translucent windows, drop shadows, AA rounded rects — on the existing
  `microui` path, for free once blend + AA exist.
- **Vector graphics / a Lua canvas:** paths, fills, gradients, AA strokes.
- **raylib (2D):** the textured-triangle + transform core *is* the missing
  renderer; 2D raylib becomes a platform shim on top.
- **A general 3D engine:** the triangle rasterizer plus a z-buffer (a later add).
- **Less code:** four packers and three blit loops collapse to one each.

## Open decisions (want a call before phase 2)

1. **Blend as a per-call argument vs. surface state.** Per-call
   (`gfx_fill_blend(..., GFX_OVER)`) is stateless and simple; surface-state
   (GL-like `gfx_set_blend(s, mode)`) reads cleaner for the triangle/rlgl path.
   *Proposed: per-call for primitives, transform+blend as surface state for the
   triangle layer* (matches how rlgl batches).
2. **Namespace.** Keep growing `gfx_*`, or split the pure rasterizer into a
   `raster_*` module that `gfx` (the display/compositor layer) sits on top of?
   *Proposed: keep `gfx_*`* — one surface type, least churn — and treat "display"
   (screen, flip, mode-set) as a thin sub-area of the same module.
3. **Color width everywhere.** Commit the whole API to `0xAARRGGBB` (alpha 0 =
   opaque for legacy callers), or keep `0xRRGGBB` and pass coverage/alpha
   separately? *Proposed: `0xAARRGGBB`, legacy opaque* — one color type, blend is
   just "alpha < 255."
4. **How far in this pass.** Everything through phase 3, or stop after phase 2
   (de-dup + blend + AA on the primitive layer) and treat the triangle/rlgl core
   as its own follow-up tied to the raylib decision?

## Non-goals (for this library)

- GPU / hardware acceleration (the real target has none we can drive).
- Gamma-correct / linear-space blending (do naive 8-bit `over` first).
- Perspective-correct texturing and a z-buffer (that's the 3D-engine note).
- Reentrant / multi-core drawing (BSP-only, like the rest of graphics today).
