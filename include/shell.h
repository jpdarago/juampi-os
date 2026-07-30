#ifndef __SHELL_H
#define __SHELL_H

#include <memory.h>

// Interactive kernel shell. On a framebuffer it runs the windowed desktop; over
// serial (headless) it runs the classic line REPL. `heap` is the root
// allocator, handed down so the UI never reaches for a global one (per-widget
// arenas are carved from it).
void shell_run(struct heap_allocator* heap);

#endif
