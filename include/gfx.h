#ifndef __GFX_H
#define __GFX_H

#include <limine.h>

#include <stdint.h>
#include <stdbool.h>

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
void gfx_pixel(int64_t x, int64_t y, uint32_t rgb);
void gfx_rect(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t rgb);
void gfx_clear(uint32_t rgb);
void gfx_line(int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint32_t rgb);

// Blit a width*height array of 0xAARRGGBB pixels with its top-left at (x, y).
// Fully transparent pixels (alpha 0) are skipped, so images with a cut-out
// background compose onto whatever is already on screen; any other alpha is
// treated as opaque.
void gfx_blit(int64_t x, int64_t y, uint64_t width, uint64_t height,
              const uint32_t* pixels);

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
void gfx_flip(void);

#endif
