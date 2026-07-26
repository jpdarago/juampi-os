---
title: Desktop UI & windowing
tags: [design, ui, microui, reentrancy, smp, needs-work]
status: in progress
milestone: M11
related: ["[[lua-shell]]", "[[x86-64-port]]", "[[Index]]"]
---

# Desktop UI & windowing

> [!abstract] Goal
> A windowed desktop for the ring-0 Lua machine: the shell in a terminal window,
> coexisting help/reference/tool windows, a canvas for graphics, and a modal vim
> editor. Built on the vendored [rxi/microui](https://github.com/rxi/microui)
> immediate-mode library.

> [!danger] Design concern this note addresses
> The UI as built is a **pile of module-level singletons** driven on the BSP.
> There is one microui context, one framebuffer target, one clip rect, one
> terminal, one editor, and several shared flags. That makes it **not reentrant**
> (no two terminals/editors, fragile nesting) and **not thread-safe** (no locking
> around the shared framebuffer). The target design below moves to **per-instance
> context handles** so the UI is reentrant, with a clear (later) path to a
> multithreaded compositor.

## What we built

- **microui** vendored (`src/microui/`), rendered by hand through clip-aware gfx
  primitives (`gfx_fill`/`gfx_text`/`gfx_glyph`/`gfx_clip`/`gfx_image`) into the
  double-buffered framebuffer (`gfx.c`).
- **Desktop loop** `ui_desktop_run()` — the top-level surface: pump PS/2
  mouse+keyboard, build the terminal window + registered windows, composite, flip.
  Replaces the old blocking REPL when a framebuffer is present.
- **Terminal** (`term.c`) — a scrollback cell grid fed by a console sink, plus a
  highlighted input line driving `luashell_eval`.
- **Windows** — modal (`ui.window`, `ui.popup/alert/confirm`) run a nested
  `ui_run` loop; non-modal (`ui.open`) register in a table the desktop renders.
- **Canvas** (`ui.canvas`, `gfx_target`) — off-screen surfaces for graphics demos
  and native `raytracer.elf`, shown in windows.
- **Editor** (`editor.c`) — a modal vim-style editor window (`ui_edit`).
- **File browser** (`files()` in `prelude.lua`).

See the git log (`ui:` / `editor:` commits) for the build order.

## Major decisions (as built)

1. **Immediate-mode, renderer-agnostic.** microui accumulates a command list we
   translate to gfx. *Good call* — tiny, no retained widget tree, easy to render
   with our 8×16 font. It does not by itself force globals.
2. **One global microui context** (`g_ctx` in `ui.c`, lazily `mu_init`'d).
   `ui_current()` returns it while `in_frame` is set. *This is the root
   singleton*: the entire UI is one microui instance, so only one build pass can
   be live at a time (which is also why nested `mu_begin` is illegal — see the
   deferral rule).
3. **gfx is a global framebuffer state machine.** `fb/width/height/pitch/shifts`,
   the `back` buffer, the `target` + saved geometry (`save_w/h/pitch`), the clip
   rect (`cl_*`), and the `snap` buffer are all module statics. `gfx_target()`
   *swaps* the geometry into one saved slot — **not nestable**.
4. **Modal vs persistent loops.** `ui_run(build, ud)` is a modal loop; the desktop
   is a persistent one. Both drive the same `g_ctx` and share the input statics
   (`cur_x/cur_y`, `prev_btn`, `esc_state`). Safe only because they never run
   concurrently and never truly nest.
5. **Widgets are singletons.** `term.c` has one grid + one input line + one
   history; `editor.c` has one `line_buf` + one cursor + one vim/undo state. So
   there can be exactly one terminal and one editor at a time.
6. **Registries, not handles.** `ui.open` windows live in a fixed `deskw[]` array
   keyed by title; canvas windows in `canv[]`. Lua refers to windows by integer
   id, not an object.
7. **Deferral rule.** You cannot open a nested modal (`edit`, `ui.window`) from
   inside a window's per-frame callback (that's mid-`mu_begin`/`mu_end`). The file
   browser works around this by recording the pick, calling `ui.close()`, and
   acting after the modal returns. *This is a direct symptom of the single-context
   design.*

## The global-state inventory

| File          | State | Why it exists | Consequence |
| ------------- | ----- | ------------- | ----------- |
| `ui.c`        | `g_ctx`, `in_frame` | the one microui context | whole UI is single-instance; no nested build |
| `ui.c`        | `cur_x/cur_y`, `prev_btn`, `esc_state` | cursor + input decode | shared by every loop; not per-session |
| `gfx.c`       | `fb/width/height/pitch/shifts`, `back`, `snap` | the one screen | one drawing target for the machine |
| `gfx.c`       | `target`, `save_w/h/pitch`, `target_dirty` | off-screen redirect | **`gfx_target` cannot nest** |
| `gfx.c`       | `cl_x0..cl_y1` | current clip | single rect (fine for the sequential command-list renderer, not for reentrant direct drawing) |
| `term.c`      | `grid`, `in/history/…`, `oesc/kesc` | the terminal | exactly one terminal |
| `editor.c`    | `line_buf`, `cx/cy`, `vmode`, `vundo[]`, `vyank` | the editor | exactly one editor; `edit()` is not reentrant |
| `lua_ui.c`    | `deskw[]`, `canv[]`, `desk_gid`, `modal_close` | window registries | fixed capacity, id-keyed |
| `mouse.c`     | `acc_dx/dy`, `buttons` (`volatile`) | the PS/2 device | fine — it *is* one device; guarded by `cli/sti` |

No `spinlock`/atomics anywhere in `ui.c`/`term.c`/`editor.c`/`gfx.c`. Only
`console.c` (`console_lock`) and `mouse.c` (`cli/sti`) are concurrency-aware.

## Assessment vs. the goal

> [!warning] Reentrancy — currently **no**
> - One `g_ctx` ⇒ one live build pass; nested `mu_begin` is illegal, hence the
>   deferral rule and the "no window opens another window directly" limitation.
> - `gfx_target` has a single saved-geometry slot ⇒ a canvas draw inside another
>   canvas draw corrupts the screen geometry.
> - `modal_close`, `esc_state`, `cur_x/cur_y` are shared across the desktop loop,
>   `ui_run`, and `ui_edit`; correct today only because those loops are strictly
>   sequential.
> - `term.c` and `editor.c` are singletons ⇒ no second terminal, no second
>   editor, `edit()` inside `edit()` would clobber the buffer.

> [!danger] Thread-safety — currently **no**
> The whole UI runs on the **BSP**. There is no lock on `gfx` state or `g_ctx`.
> This is consistent with the [[lua-shell]] SMP model (APs run **compute-only**
> `lua_State`s with no `fb`/`console`/UI access), but it means the UI cannot today
> be driven from a worker core, and two producers cannot draw concurrently.

> [!note] It "works" because of discipline, not structure
> Everything is serialized on one core and the loops never overlap. The bugs we
> hit — the `dd`-empties-buffer case, the reopen-stays-closed container flag, the
> Enter-not-splitting key mapping — were logic bugs, but the *fragility* (shared
> flags, single context, defer-or-crash nesting) is structural.

## Target design

The through-line: **replace module statics with explicit context handles passed
by the caller.** Nothing that represents "an instance of a thing" should be a
file-scope `static`.

### 1. gfx → a `surface` abstraction
A `gfx_surface` bundles `{pixels, w, h, pitch, shifts, clip-stack}`. The screen
(front/back) is one surface; every canvas is a surface. Drawing takes a surface:
`gfx_fill(surf, ...)`, `gfx_text(surf, ...)`. Clip becomes a **stack** on the
surface (`gfx_clip_push/pop`) mirroring microui's own clip stack, which makes
direct drawing reentrant and removes `gfx_target`/`save_*` entirely (you just
draw to a different surface).

### 2. microui context per UI session
Allocate a `mu_Context` per top-level loop (desktop, each modal) instead of one
`g_ctx`. `ui_current()` becomes a small explicit stack of active contexts (or the
context is threaded through the build callbacks). This legalizes nesting: a window
*can* open a child window/dialog, so the deferral rule goes away.

### 3. Widgets as heap instances
`term_t* term_new()`, `editor_t* editor_open(path)`; all the `term.c`/`editor.c`
statics become struct fields. Now multiple terminals and multiple editor windows
are possible, and `edit()` is reentrant. The desktop keeps a **list of windows**,
each owning its widget instance and a build callback — replacing `deskw[]`/`canv[]`
with one uniform window type.

### 4. Input + cursor per desktop, not global
Fold `cur_x/cur_y`, `prev_btn`, and the escape-decoder state into the desktop/loop
context. Route keys to the **focused** window's handler rather than a single
`term_key`.

### 5. Threading model (later milestone)
The framebuffer is inherently one shared resource, so "multithreaded UI" means a
**compositor**, not lock-free free-for-all:

- Keep one **compositor** (owns the screen surface + flip), running on the BSP.
- Worker cores build into their **own** microui context + off-screen surface
  (now that both are per-instance) and **submit** the surface to the compositor
  through a lock (or an SPSC queue), which blits it into a window. This fits the
  [[lua-shell]] per-core-state model: each core already has isolated state; only
  the submit/compose step needs a `spinlock`.
- Minimum viable step: put a `spinlock` around gfx-surface mutation of the screen
  so `console`/UI output from any core is at least safe, matching `console_lock`.

> [!note] Recommendation
> Do **reentrancy first** (steps 1–4): it removes the real fragility, enables
> multiple windows/editors, and deletes the deferral hack — all without a
> threading model. Treat the **compositor + per-core surfaces** (step 5) as its
> own milestone, since a genuinely multithreaded UI is only meaningful once
> contexts and surfaces are per-instance.

## Open decisions

- [ ] **Context threading vs. context stack.** Pass `mu_Context*`/`surface*`
  explicitly through every draw call (clean, verbose, touches the whole Lua
  binding surface) **or** keep an implicit "current context" stack set by the
  loop (smaller diff, still a hidden global but a *stack*, so nesting works).
  *Lean: explicit handles in C, a thin implicit "current" for the Lua `ui.*`
  widget wrappers so `ui.button(...)` stays argument-free.*
- [ ] **How far to push multithreading.** Full per-core UI producers, or just a
  screen lock + BSP-only building? *Lean: screen lock now, per-core producers
  only if a real use case appears (the compute model rarely needs worker-drawn
  windows).*
- [ ] **Window model.** Unify `deskw[]` (Lua) + `canv[]` (C) + terminal + editor
  into one `window` type with an owner + build callback + optional widget
  instance.
- [ ] **Capacity.** Fixed arrays (`MAXW=8`, `MAXCANV=4`) vs. a heap list.

## Migration plan (incremental, each build/lint/smoke-clean)

0. ✅ **Allocator injection** (`b02ae54`): `kmain → shell_run(heap) → ui_init`;
   the UI never calls `heap_default()`; `ui_root_heap()` is the seam widgets'
   arenas are carved from.
1. Introduce `gfx_surface` and convert `gfx_*` to take one; make the screen and
   canvases surfaces; replace `gfx_target` with "draw to a surface" + a clip
   stack. (No API change visible to Lua yet.)
2. ✅ Make `term`/`editor` heap instances; keep one desktop terminal for now.
   - ✅ **Editor** (`0a5a03f`): `struct editor` instance, opened from a 2 MiB
     per-widget arena (`ui_arena_new/free` in ui.c) and freed wholesale; zero
     file-scope statics; undo = fixed-slot ring, yank = fixed slot, save
     serializes into the reused frame scratch (no churn — arenas can't free).
     Gotcha: the heap's max alignment is **16**; asking for 64 panics.
   - ✅ **Terminal** (`9425ed3`): `struct term` instance from a 1 MiB
     desktop-lifetime arena (`desk_term` in ui.c); the ~430 KB grid + input +
     history leave the BSS. Needed a **context-carrying console sink**
     (`console_set_sink(fn, ctx)`) so the sink is the instance, not a global.
3. Per-session `mu_Context` + a context stack; drop the deferral rule; let a
   window open a child window.
4. Unify the window registries; route input to the focused window.
5. (Milestone M12) Screen `spinlock`; optional per-core surfaces + compositor
   submit.

## Related

[[lua-shell]] (SMP + per-core state model this must fit), [[Index]].
