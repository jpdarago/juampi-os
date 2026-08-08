#ifndef __GFX_H
#define __GFX_H

#include <limine.h>

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Direct drawing to the Limine framebuffer (32-bit only). Coexists with the
// flanterm text console on the same surface — graphics and text overwrite each
// other pixel by pixel. Colours are 0xRRGGBB, repacked to the framebuffer's
// channel layout. Off-screen coordinates are clipped.
void gfx_init(struct limine_framebuffer* fb);
bool gfx_available(void);
uint64_t gfx_width(void);
uint64_t gfx_height(void);
uint64_t gfx_pitch(void); // bytes per scanline (may exceed width*4)
// The framebuffer's 0xRRGGBB channel bit shifts, so callers packing pixels
// themselves (e.g. a parallel renderer over fb.canvas) match the current mode.
void gfx_shifts(uint8_t* r, uint8_t* g, uint8_t* b);
// The live framebuffer base; *size (bytes) and *pitch describe it. NULL if
// headless.
void* gfx_framebuffer(uint64_t* size, uint64_t* pitch);

// Switch the display to width*height at 32bpp at runtime (QEMU stdvga / Bochs
// DISPI). Re-points graphics and the console at the new geometry. Returns false
// if there is no framebuffer, no DISPI, or the mode is out of range.
bool gfx_set_mode(uint32_t width, uint32_t height);

// A drawing surface: a pixel buffer with its own geometry, channel layout, and
// clip rectangle. Every drawing primitive takes one explicitly, so a canvas
// draw can't disturb the screen and nothing lives in a global. It is a plain
// value type — construct one over a packed buffer with gfx_surface_make(), or
// get the screen's with gfx_screen(). `pitch` is in bytes; the clip is an
// exclusive [cx0,cx1) x [cy0,cy1) box (INT64_MAX = whole surface).
struct gfx_surface {
    uint8_t* pixels;
    uint64_t w, h, pitch;
    uint8_t r_shift, g_shift, b_shift;
    int64_t cx0, cy0, cx1, cy1;
};

// The one place channel packing lives. `gfx_pack_rgb` packs a 0xRRGGBB colour
// into a native framebuffer pixel with the given channel shifts;
// `gfx_unpack_rgb` is the inverse. Every surface primitive and the
// hosted-present blit go through these instead of open-coding
// `(r<<rs)|(g<<gs)|(b<<bs)`. Inline so there's no call cost in the per-pixel
// loops.
static inline uint32_t gfx_pack_rgb(uint32_t rgb, uint8_t rs, uint8_t gs,
                                    uint8_t bs)
{
    return (((rgb >> 16) & 0xFF) << rs) | (((rgb >> 8) & 0xFF) << gs) |
           ((rgb & 0xFF) << bs);
}
static inline uint32_t gfx_unpack_rgb(uint32_t native, uint8_t rs, uint8_t gs,
                                      uint8_t bs)
{
    return (((native >> rs) & 0xFF) << 16) | (((native >> gs) & 0xFF) << 8) |
           ((native >> bs) & 0xFF);
}

// Blend modes. Colours are 0xAARRGGBB; a 0 alpha byte (i.e. a legacy 0xRRGGBB)
// is read as opaque, so "translucent" is simply "alpha < 255." GFX_COPY
// overwrites (the default for opaque draws); GFX_OVER is src-over-dst by the
// source alpha; GFX_ADD is additive (glows); GFX_MUL modulates (tints/shadows).
enum gfx_blend { GFX_COPY, GFX_OVER, GFX_ADD, GFX_MUL };

// The screen as a surface (the back buffer when double-buffering, else the
// hardware framebuffer). Its clip persists across calls; the UI renderer sets
// it per microui CLIP command. NULL if headless.
struct gfx_surface* gfx_screen(void);

// Build a surface over a tightly packed (pitch = w*4) native-layout buffer,
// taking the screen's channel shifts and a full (unclipped) clip box.
struct gfx_surface gfx_surface_make(uint32_t* pixels, uint64_t w, uint64_t h);

// Drawing primitives, all into an explicit surface, all clipped to the
// surface's clip box and bounds. gfx_clip sets that box; gfx_clip_reset drops
// back to the whole surface.
void gfx_clip(struct gfx_surface* s, int64_t x, int64_t y, int64_t w,
              int64_t h);
void gfx_clip_reset(struct gfx_surface* s);
void gfx_pixel(struct gfx_surface* s, int64_t x, int64_t y, uint32_t rgb);
void gfx_rect(struct gfx_surface* s, int64_t x, int64_t y, int64_t w, int64_t h,
              uint32_t rgb);
void gfx_clear(struct gfx_surface* s, uint32_t rgb);
void gfx_line(struct gfx_surface* s, int64_t x0, int64_t y0, int64_t x1,
              int64_t y1, uint32_t rgb);
void gfx_fill(struct gfx_surface* s, int64_t x, int64_t y, int64_t w, int64_t h,
              uint32_t rgb);
// The blend engine: plot one clipped pixel, blending `argb` (0xAARRGGBB) at
// (x,y) under `blend`, with `coverage` in [0,255] scaling the source alpha — so
// anti-aliased glyph/edge coverage feeds the same path. Every translucent draw
// bottoms out here; gfx_glyph/gfx_fill's opaque paths stay their own fast
// loops.
void gfx_plot(struct gfx_surface* s, int64_t x, int64_t y, uint32_t argb,
              unsigned coverage, enum gfx_blend blend);
// Blended rectangle fill: like gfx_fill but honours the source alpha + blend
// mode. Opaque (alpha 255 / legacy 0xRRGGBB, GFX_OVER|COPY) takes gfx_fill's
// fast path; otherwise it's a per-pixel blend.
void gfx_fill_blend(struct gfx_surface* s, int64_t x, int64_t y, int64_t w,
                    int64_t h, uint32_t argb, enum gfx_blend blend);
// Draw one 8x16 glyph / an n-byte string at pixel (x, y), honoring the clip.
void gfx_glyph(struct gfx_surface* s, int64_t x, int64_t y, unsigned char c,
               uint32_t rgb);
void gfx_text(struct gfx_surface* s, int64_t x, int64_t y, const char* str,
              size_t n, uint32_t rgb);

// Save / restore the current draw target (back buffer when buffered, else the
// screen) into a heap snapshot. The UI loop snapshots the shell image once,
// then restores it under the windows each frame so popups float over live text.
void gfx_snapshot(void);
void gfx_restore(void);
void gfx_snapshot_free(void);

// Blit a native-layout w*h buffer (a Lua canvas) into surface `s` at (x, y),
// 1:1 and clip-aware, to paint a canvas window.
void gfx_image(struct gfx_surface* s, int64_t x, int64_t y, int64_t w,
               int64_t h, const uint32_t* buf);

// Blit a width*height array of 0xAARRGGBB pixels into surface `s` with its
// top-left at (x, y). Fully transparent pixels (alpha 0) are skipped, so images
// with a cut-out background compose onto what is already there; any other alpha
// is treated as opaque.
void gfx_blit(struct gfx_surface* s, int64_t x, int64_t y, uint64_t width,
              uint64_t height, const uint32_t* pixels);

// Double buffering. gfx_buffer(true) allocates an off-screen back buffer and
// redirects all subsequent drawing to it (seeded with the current screen);
// gfx_buffer(false) frees it and returns to drawing straight to the screen.
// While buffered, nothing appears until gfx_flip() copies the back buffer to
// the framebuffer in one pass — so animations never show a half-drawn frame.
// Both return / report whether buffering is now active (false if there is no
// framebuffer). Text drawn by the console goes to the screen directly and is
// therefore overwritten by the next flip.
bool gfx_buffer(bool on);
bool gfx_buffered(void);
// gfx_flip() is damage-tracked: it diffs the back buffer against a shadow of
// the framebuffer and copies only the changed tiles to VRAM (an idle desktop
// flushes a handful instead of the whole screen). gfx_flip_full() forces a
// whole-screen copy (used after a mode change). gfx_flip_tiles() reports how
// many tiles the last flip pushed — 0 means the frame was identical — for
// tests/benchmarks.
void gfx_flip(void);
void gfx_flip_full(void);
uint32_t gfx_flip_tiles(void);

#endif
