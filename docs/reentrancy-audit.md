---
title: Reentrancy & allocation audit
tags: [design, audit, reentrancy, smp, allocator, ui, needs-work]
status: in progress
milestone: M11
related: ["[[ui]]", "[[lua-shell]]", "[[Index]]"]
---

# Reentrancy & allocation audit

> [!abstract] Scope
> A whole-kernel review of **mutable state, reentrancy, SMP safety, allocator
> discipline, and the UI API** (Lua + native ELF). Goal from the user: avoid
> mutable state where possible, pass allocators explicitly, avoid
> statics/globals/hardcoded memory, be reentrant and multi-thread-safe. The UI
> half continues in [[ui]]; this note is the cross-cutting inventory + plan.

## Bottom line

The kernel is sound for what it is. The SMP model — one `lua_State` + one heap
per core, **all IO on the BSP** — is coherent, and the newer subsystems already
follow the target discipline: `http_get(allocator*)`, `qoi_decode(allocator*)`,
`editor_open(allocator*)`, `smp_init/sched_init/gdt_init(allocator*)`, and the
non-owning `str` views. The target style already exists in-tree; the work is
bringing older code up to it.

Three honest framings:

1. **Almost no live multicore bug.** APs never touch IO/console/fb/net/disk, so
   the many "no spinlock" findings are *latent*, not *critical*. Don't rush to
   add locks — keep state per-instance and passed-in so locking stays a
   localized decision at the compositor step ([[ui]]).
2. **The real reentrancy smells are function-level static scratch buffers**, not
   the singletons.
3. **The biggest allocation-discipline lever is ext2**, not the UI.

## Severity legend

- **BUG** — wrong if used reentrantly even on one core (nested/re-entered call).
- **LATENT** — safe only by the BSP-only convention; no code guard.
- **STYLE** — avoidable static/global/hardcoded; not a bug.

## 1. Reentrancy & mutable state

> [!warning] Function-level static scratch — fix these (STYLE→BUG-shaped, cheap)
> Reused static buffers that a nested/second call would clobber:
> - `src/udp.c:66` — `static uint8_t seg[...]` built then handed to `ip_send`.
> - `src/net.c:172` (`frame[1600]`), `:254` (`pkt[1600]`), `:308` (`rep[1500]`).
> - `src/lua/lua_net.c:199` (`buf[1472]`), `:322` (`buf[4096]`) — recv scratch
>   returned to Lua.
>
> None is live today (all BSP, non-nested) but all contradict "avoid statics"
> and are the fragile kind. Make them stack-local or per-call arena.

**LATENT singletons (safe today, no guard) — document, don't refactor:**
`arp_cache`, `tcp_conns[2]`, `udp_socks[8]`, ephemeral-port counter, ping state
(`net.c`/`tcp.c`/`udp.c`); ext2 mount state (`ext2.c:100`); gfx state (`gfx.c`);
frame bitmap (`frames.c`); scheduler `current` (`sched.c`); Lua `L` + `pending`
(`lua_glue.c`). These are legitimately one-per-machine / one-per-BSP. A one-line
"BSP-only, unlocked" comment is the right treatment.

**Already correct:** keyboard ring (single producer/consumer), mouse (`cli/sti`),
console (`console_lock`), SMP mailboxes (atomics). One gap:
`console_set_sink()` writes `extra_sink` **outside** the lock
(`src/console.c:75`) — torn-pointer risk; move under `console_lock`.

## 2. Allocation discipline

45 `heap_default()` sites. Distribution:

| File | sites | assessment |
| ---- | ----- | ---------- |
| `src/ext2.c` | 19 | **the big one** — `mem()` (ext2.c:107) returns `heap_default()`; the whole API (`ext2_read_path`/`ext2_read`/`ext2_write_file`) takes no allocator |
| `src/lua/lua_run.c` | 6 | script load/exec buffers should come from a caller arena |
| `src/gfx.c` | 5 | back/target/snap — folds into the `gfx_surface` step ([[ui]]) |
| `klibc.c` / `lua_thread.c` | 3 / 2 | **legit** — this *is* the Lua malloc shim |
| `editor.c` / `lua_edit.c` / `lua_fs.c` | 1 each | **legit** — freeing the blob `ext2_read_path` returns (fs contract) |

Highest-value change: **thread an `allocator*` through ext2**
(`ext2_read_path(allocator*, …)`, `mem()` → passed handle) — 19 sites, unblocks
the Lua fs layer from the global heap, makes the fs reusable. Follow the
`http.c`/`editor.c` template. `lua_run` and `gfx` next; the malloc shim stays on
the global heap by design.

## 3. Hardcoded caps

**Inherent (keep):** x86-64 VAs (`KHEAP_START`, `NICWIN_VA`), page/slab sizes,
protocol limits (`UDP_MSG_MAX=1472`, `TCP_MSS=536`), font metrics. **Arbitrary,
static-table-backed (revisit):** `TCP_CONNS=2` (only two sockets total),
`UDP_SOCKETS=8`, `MAX_THREADS=8`, terminal `1000×220` (~880 KB static grid —
the next instance conversion in [[ui]]), `MAXCANV=4`/`MAXW=8`. None a bug; make
the blocking ones (esp. `TCP_CONNS`) larger or allocator-backed lists.

## 4. UI API (Lua + ELF)

**Good:** immediate-mode maps cleanly to Lua; `need_ctx()` (`lua_ui.c:29`) errors
clearly outside a frame; modal + persistent windows both work; canvas userdata
has `__gc`.

> [!warning] Rough edges
> - **Two things named "canvas", opposite semantics.** `ui.canvas(w,h)`
>   *allocates* off-screen; `fb.canvas()` *borrows* the live framebuffer. Rename
>   toward `ui.surface()` (owned) vs `fb.framebuffer()` (view) with the surface
>   refactor.
> - **`fb.*` and `ui.*` silently share global gfx state** — `fb.rect()` inside a
>   window callback draws through the borrowed target/clip with no warning.
> - **Deferral rule leaks to users** — `if ui.button() then edit(f) end` inside a
>   callback crashes/hangs; goes away with per-session contexts.
> - **Missing widgets:** no `ui.textbox` (microui has `mu_textbox_ex` — biggest
>   gap), no list/menu, no `ui.column`/layout beyond `ui.row`, no window
>   positioning, no per-window state helper.
> - **Headless dialogs silently no-op** instead of degrading to console I/O.

**ELF/lab binaries are second-class for graphics.** `lab_api` (`include/lab.h:22`)
gives raw framebuffer + `alloc/free/print/run_on/join` — enough for a fullscreen
raytracer, but native binaries **cannot** get a windowed canvas, emit widgets, or
read input; they run once and return a `long`. `api->alloc/free` is ad-hoc, not
an `alloc.h` allocator.

**Convergence (payoff of `gfx_surface`):** once a surface is
`{pixels,w,h,pitch,shifts,clip-stack}` and drawing takes a surface, both
consumers unify — Lua's two canvases become one surface type, and `lab_api` can
hand native binaries a real surface (allocate → render → return for a window)
plus an `allocator*`. That's the clean way to let ELF binaries join the desktop.

## Recommended sequence (interleaves with [[ui]])

1. ☐ **Cheap reentrancy hygiene** — destatic net/udp/lua_net scratch (§1); move
   `console_set_sink` under the lock. Small independent commits.
2. ☐ **ext2 takes an allocator** (§2) — highest-value discipline change.
3. ☐ **Continue the UI plan** ([[ui]]): terminal instance → `gfx_surface` (also
   clears the 5 gfx `heap_default` sites + the `fb`/`ui` shared-state edge) →
   per-session contexts (kills the deferral rule) → `ui.textbox` + canvas rename
   + `lab_api` surface/allocator for ELF.
4. ☐ **Defer locking** to the compositor milestone; until then one-line
   "BSP-only, unlocked" comments on the latent singletons.

## Related

[[ui]] (the UI half of this + the migration plan), [[lua-shell]] (SMP model),
[[Index]].
