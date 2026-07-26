#ifndef __UI_H
#define __UI_H

#include <stdbool.h>
#include <stdint.h>

// Graphical UI layer: renders the vendored microui (src/microui/) over the
// shell framebuffer and drives it with the PS/2 mouse + keyboard. Popups run as
// a modal loop (like the full-screen editor) — they float over the shell text
// and restore it on close. See src/ui.c; the Lua `ui` library is lua_ui.c.

struct mu_Context;

// True when a framebuffer + microui context are available (the UI needs one).
bool ui_available(void);

// Run the persistent windowed desktop: the shell lives in a terminal window,
// help/tool windows coexist alongside it. Never returns. No-op (so the caller
// falls back to the classic text REPL) when there is no framebuffer.
void ui_desktop_run(void);

// Bracket a full-screen activity (a framebuffer demo) that must own the raw
// screen: begin suspends the desktop compositor (output reverts to the flanterm
// console); end resumes it. No-ops without a framebuffer.
void ui_fullscreen_begin(void);
void ui_fullscreen_end(void);

// Run the windowed vim-style editor on `path` as a modal window over the
// desktop (or the classic full-screen editor when headless). Returns
// EDITOR_RUN/QUIT.
int ui_edit(const char* path);

// Draw an ANSI-SGR-colored string as colored text runs at pixel (x, y) inside a
// window (used by the editor's highlighted lines).
void ui_text_ansi(struct mu_Context* ctx, const char* s, int x, int y);

// Per-frame build callback: called once each frame between mu_begin/mu_end to
// populate the UI (open windows, emit widgets). Return false to close the loop.
typedef bool (*ui_frame_fn)(struct mu_Context* ctx, void* ud);

// Run the modal UI loop, calling `build` every frame until it (or Esc / a
// closed window) asks to stop. No-op without a framebuffer.
void ui_run(ui_frame_fn build, void* ud);

// The microui context while a frame is building, else NULL. The Lua widget
// wrappers (lua_ui.c) operate on this.
struct mu_Context* ui_current(void);

// Register a callback the desktop loop invokes each frame (between the terminal
// window and the flip) to build non-modal windows. lua_ui.c uses this to render
// windows opened with ui.open(). Pass NULL to detach.
void ui_set_window_hook(void (*fn)(struct mu_Context*));

// Blit a native-layout w*h pixel buffer (a Lua canvas) into the current
// window's body, 1:1. Valid only inside a window build callback. See ui.canvas
// / cv:show.
void ui_image(struct mu_Context* ctx, const uint32_t* buf, int w, int h);

// Open a persistent desktop window showing a native-layout w*h pixel buffer (a
// native lab program that rendered off-screen). Takes ownership of `buf` (freed
// when the window is closed); re-using a title re-renders into the same window.
void ui_open_canvas(const char* title, uint32_t* buf, int w, int h);

// Native convenience popups (used by the Lua `ui` library and help()).
void ui_message(const char* title, const char* body); // scrollable text window
bool ui_confirm(const char* title, const char* body); // OK/Cancel -> true/false

#endif
