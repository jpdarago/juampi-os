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
void gfx_pixel(int64_t x, int64_t y, uint32_t rgb);
void gfx_rect(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t rgb);
void gfx_clear(uint32_t rgb);
void gfx_line(int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint32_t rgb);

#endif
