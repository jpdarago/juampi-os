#ifndef __EDITOR_H
#define __EDITOR_H

#include <alloc.h>

// Text editor for the kernel shell, as a reentrant instance: editor_open()
// allocates every buffer (document, undo ring, yank register, render scratch)
// from the allocator the caller provides — by convention a per-widget arena
// whose wholesale free ends the editor's lifetime, so there is no editor_close.
// Multiple instances may exist at once; an instance is single-owner (drive it
// from one core), and the shared console/ext2/gfx services remain the
// machine-wide serialization boundary (see docs/ui.md).
//
// Two frontends over the same instance:
//   * editor_run(e)      — classic blocking full-screen ANSI editor (headless).
//   * editor_vim_key(e, byte) / editor_vim_draw(e, ctx) — the windowed
//     vim-style editor, pumped and rendered per frame by ui_edit() (src/ui.c).

struct editor;
struct mu_Context;

// EDITOR_CONTINUE means the editor is still running (returned by
// editor_vim_key between keystrokes); RUN means "save succeeded, execute the
// file"; QUIT returns to the shell.
enum { EDITOR_QUIT = 0, EDITOR_RUN = 1, EDITOR_CONTINUE = 2 };

// Load `path` (missing file = empty buffer) into a fresh instance whose memory
// all comes from `mem`. Never fails (out-of-memory panics, per alloc.h).
struct editor* editor_open(struct allocator* mem, const char* path);

// Classic full-screen ANSI editor loop (headless / serial fallback).
int editor_run(struct editor* e);

// Windowed vim editor: feed one input byte (or -1 to flush a dangling Esc at
// end of frame) and render into the current microui window.
int editor_vim_key(struct editor* e, int byte);
void editor_vim_draw(struct editor* e, struct mu_Context* ctx);

#endif
