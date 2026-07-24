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
// caller can execute the file; otherwise EDITOR_QUIT.
enum { EDITOR_QUIT = 0, EDITOR_RUN = 1 };

int editor_run(const char* path);

#endif
