#ifndef __TERM_H
#define __TERM_H

#include <alloc.h>

// The windowed terminal, as a reentrant instance: a scrollback grid fed by
// console output plus a live, syntax-highlighted input line that drives the Lua
// REPL. term_open() allocates all state (the grid, input line, history, escape
// decoders) from the caller's allocator — by convention a per-widget arena, so
// there are no file-scope statics and more than one terminal can exist.
//
// The desktop shell (src/ui.c): term_open() once, install term_write as the
// console sink (with the instance as ctx) so shell output lands in the
// scrollback, feed keystrokes with term_key, and draw the window each frame
// with term_build. See src/term.c.

struct term;
struct mu_Context;

// Create a terminal whose memory all comes from `mem`.
struct term* term_open(struct allocator* mem);

// Console sink: append one output byte to the scrollback. `ctx` is the term*
// (the signature matches console_set_sink's callback).
void term_write(void* ctx, char c);

// Feed one input byte (non-blocking, from the pump).
void term_key(struct term* t, int c);

// Render the terminal window this frame.
void term_build(struct term* t, struct mu_Context* ctx);

#endif
