#ifndef __THEME_H
#define __THEME_H

#include <stdint.h>

// Shared UI colors (0xRRGGBB) so the GUI terminal (src/term.c), the windowed
// editor (src/editor.c) and the microui highlighter (src/ui.c) stay visually
// consistent instead of each hardcoding its own copy.

#define THEME_BG 0x0e1116     // window / editor background
#define THEME_CURSOR 0x9ecbff // text cursor

// The colors the Lua syntax highlighter emits, indexed by its SGR code:
// default foreground, green (keywords), yellow (strings), magenta (numbers),
// grey (comments). term.c and ui.c both render highlighted text with this.
#define THEME_HL_COUNT 5
#define THEME_HL_PALETTE {0xd4d4d4, 0x6ac46a, 0xd4c46a, 0xc46ac4, 0x808080}

#endif
