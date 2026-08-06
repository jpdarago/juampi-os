#ifndef __TTF_H
#define __TTF_H

#include <stdint.h>
#include <stddef.h>
#include <memory.h>
#include <gfx.h>

// A scalable, anti-aliased TrueType font rasteriser over vendored stb_truetype
// (src/stb/). Unlike the fixed 8x16 bitmap (font.h / gfx_glyph), glyphs render
// at any pixel size with coverage-based anti-aliasing and alpha-blend onto a
// gfx_surface, so text stays crisp at any scale. This is the text track of the
// software-rasterizer roadmap (docs/software-rasterizer.md); the shell, editor,
// and UI can move onto it over time.

struct ttf_font; // opaque; created by ttf_load

// Parse a TrueType font from `ttf` (`len` bytes). The bytes are copied into
// `heap`, so the caller may free its own buffer immediately. Returns NULL if
// the data is not a usable font. Later allocation (glyph bitmaps) is transient.
struct ttf_font* ttf_load(const void* ttf, size_t len,
                          struct heap_allocator* heap);

// Release a font created by ttf_load.
void ttf_free(struct ttf_font* f);

// Line advance (ascent + descent + line gap) in pixels at size `px`.
int ttf_line_height(struct ttf_font* f, float px);

// Total advance width in pixels of the ASCII string `s` at size `px`.
int ttf_text_width(struct ttf_font* f, const char* s, float px);

// Draw ASCII string `s` at size `px` (ascent-to-descent height) in colour `rgb`
// (0xRRGGBB) onto surface `dst`. (x, y) is the pen origin on the baseline;
// glyphs are alpha-blended with their coverage and clipped to the surface.
// Returns the advance width drawn (as ttf_text_width would report).
int ttf_draw(struct ttf_font* f, struct gfx_surface* dst, int x, int y,
             const char* s, float px, uint32_t rgb);

#endif
