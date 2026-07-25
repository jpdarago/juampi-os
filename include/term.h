#ifndef __TERM_H
#define __TERM_H

// The windowed terminal: a scrollback grid fed by console output plus a live,
// syntax-highlighted input line that drives the Lua REPL. Used by the desktop
// shell (src/ui.c): initialise once, install term_write as the console sink so
// shell output lands in the scrollback, feed keystrokes with term_key, and draw
// the terminal window each frame with term_build. See src/term.c.

struct mu_Context;

void term_init(void);    // reset the grid + input line (call once at startup)
void term_write(char c); // console sink: append one output byte to scrollback
void term_key(int c);    // feed one input byte (non-blocking, from the pump)
void term_build(
        struct mu_Context* ctx); // render the terminal window this frame

#endif
