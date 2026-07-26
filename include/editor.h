#ifndef __EDITOR_H
#define __EDITOR_H

// Full-screen Lua text editor for the kernel shell. Loads `path` from the ext2
// data disk (starting from an empty buffer if it does not exist), lets the user
// edit it with syntax highlighting (via highlight_lua), and saves back to ext2
// with Ctrl-S. Rendering is done entirely with ANSI escapes through the
// console, so it works on the framebuffer terminal and over a serial terminal
// alike.
//
// Returns EDITOR_RUN if the user left with Ctrl-X ("save and run"), so the
// caller can execute the file; otherwise EDITOR_QUIT. EDITOR_CONTINUE means the
// editor is still running (returned by editor_vim_key between keystrokes).
enum { EDITOR_QUIT = 0, EDITOR_RUN = 1, EDITOR_CONTINUE = 2 };

// Classic full-screen ANSI editor (headless / serial fallback).
int editor_run(const char* path);

// Windowed vim-style editor, driven by the desktop loop (ui_edit in ui.c).
// editor_vim_open loads the file; editor_vim_key feeds one input byte and
// returns an EDITOR_* action; editor_vim_draw renders into the current UI
// window.
struct mu_Context;
void editor_vim_open(const char* path);
int editor_vim_key(int byte);
void editor_vim_draw(struct mu_Context* ctx);

#endif
