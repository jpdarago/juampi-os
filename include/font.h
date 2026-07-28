#ifndef __FONT_H
#define __FONT_H

// Monospace glyph cell shared by the gfx blitter and every microui view (the
// windowed terminal, editor and popups). The single source of truth for the
// 8x16 cell size; the glyph bitmaps themselves live in src/font8x16.h.
#define FONT_W 8
#define FONT_H 16

#endif
