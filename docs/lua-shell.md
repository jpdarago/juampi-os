# Booting to a parallel Lua shell

Goal: boot into an interactive Lua shell over the serial console, with a custom
library for multithreaded programming that has full access to all CPU cores and
all of physical memory.

The shell and the interpreters run in **ring 0** (kernel mode). That is what
gives "full access to all cores and all memory" for free: no syscall boundary,
Lua code can touch any physical address (via the HHDM) and dispatch work to any
core directly. This is a single-user, single-address-space machine by design —
a parallel programming appliance, not a protected multi-user OS.

## Threading model: per-core states + shared memory

Chosen model (à la Lua Lanes / SPMD): **one independent `lua_State` per core**,
running truly in parallel. `thread.spawn(core, fn)` serialises `fn` (as bytecode)
to a target core, which runs it in its own interpreter. Cores share data through
an explicit **shared-memory API** (`mem.shared(n)` → raw bytes every core can
read/write), never by sharing `lua_State`s (which are not thread-safe).

```lua
shared = mem.shared(1 << 20)          -- 1 MiB every core sees
for c = 1, ncores()-1 do
    thread.spawn(c, function()
        local base = c * (len // ncores())
        shared:u64(base, compute(base))
    end)
end
```

## Milestones (each a bootable, `make test`-validated commit)

- **M6 — Serial shell.** COM1 receive path + a line editor + a kernel command
  loop (builtins). The interactive endpoint the rest builds on.
- **M7 — Lua port.** Vendor PUC-Lua 5.4, build it freestanding (a tiny libc
  shim over the kernel allocator + string/math helpers; `io`/`os`/`package`
  stripped). Enable SSE/x87 for Lua's `double` arithmetic and save FP state on
  context switch. Wire the shell to evaluate Lua. **This is "boot to a Lua
  shell."**
- **M8 — SMP.** Limine MP request to start the application processors; per-CPU
  GDT/TSS/IDT, per-CPU data, LAPIC, and spinlocks (the kernel's `cli`-as-lock
  assumption no longer holds once APs run).
- **M9 — Parallel Lua library.** A `lua_State` per core, `thread.spawn(core, fn)`
  dispatch, `mem.shared` shared buffers, and `mem.phys`/`ncores()` primitives —
  the custom multithreading library.

## What this deliberately gives up

Protection: Lua scripts run in ring 0 and can crash or corrupt the machine.
That is the explicit trade for "full access to all cores and all memory." The
ELF64 user-mode path from the port (ring 3 + `int 0x80`) stays in the tree for
when isolation is wanted, but the Lua shell does not use it.
