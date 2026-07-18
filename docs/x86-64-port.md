# Porting juampi-os to x86-64

This document scopes the migration of the kernel from 32-bit protected mode to
64-bit long mode. It is organised as **six milestones**, each ending at a
bootable checkpoint validated under QEMU, so the tree is never left in a
long-lived uncompilable state. Work happens on the `feature/x86-64` branch; each
milestone is one commit.

## Guiding decisions

- **Bootloader: Limine.** Rather than hand-roll a long-mode trampoline, the
  kernel is booted by [Limine](https://github.com/limine-bootloader/limine), the
  modern OSDev-standard bootloader. It enters the kernel already in 64-bit long
  mode with a higher-half direct map (HHDM) set up, and hands over a clean memory
  map via its boot protocol. Crucially for the *later* multicore project, Limine
  can also start the application processors for us (its `mp`/`smp` request),
  which is the fiddliest part of SMP bringup. The kernel declares protocol
  requests in a `.limine_requests` section and reads the responses in `kmain`.
  (An earlier iteration used a hand-written PVH trampoline; Limine replaced it
  because it does more for us and is the path to SMP.)
- **Address-space layout: higher-half.** The kernel is linked at
  `0xFFFFFFFF80000000` (the top 2 GB canonical region, which `-mcmodel=kernel`
  targets) and loaded there by Limine. User space lives in the low canonical
  half.
- **Syscall mechanism: keep `int 0x80` through the port.** Only the entry stub
  and register widths change. Migrating to the `syscall`/`sysret` instructions
  is a separate, later change — rewriting the ABI and the mechanism at once is
  how you get an unbootable branch.
- **Toolchain: host GCC, no cross-compiler required.** The build host is
  x86-64, so `gcc -ffreestanding -mno-red-zone -mcmodel=kernel -fno-pic` emits
  freestanding 64-bit kernel code directly, mirroring how the 32-bit build uses
  host `gcc -m32`. `-mno-red-zone` is mandatory: the SysV red zone is unsafe in
  interrupt handlers. The `-mno-sse/mmx` set stays (the kernel saves no SIMD
  state across context switches).
- **Boot image + test loop.** `make boot.img` packs the kernel and Limine into a
  sudo-free UEFI FAT image (via `mtools`); `make run` boots it under OVMF, and
  `make test` boots it headless and greps the serial log for a milestone marker.

## The single biggest conceptual change

The 32-bit kernel switches tasks with **hardware task switching** (`jmp far`
into a TSS selector). That mechanism **does not exist in long mode.** Milestone 3
replaces it with a software context switch, and the TSS is demoted to one
per-CPU structure that only holds `rsp0` for privilege transitions.

Separately, the entire kernel uses `irq_cli()`/`irq_sti()` (interrupts off) as
its only mutual-exclusion primitive. That is correct on one CPU and is left
intact by this port; it is the thing a *later* SMP project must replace with
real locking.

---

## Milestone 0 — Limine boot into long mode  ✅ DONE

**Checkpoint:** the kernel boots via Limine into 64-bit long mode and reports the
Limine boot protocol worked. Validated by `make test` (serial marker
`Limine boot protocol OK`).

- `devenv.nix`: add `limine` and `xorriso`. (`mtools`, OVMF are already present.)
- `Makefile`: drop `-m32`; add `-mcmodel=kernel -mno-red-zone -fno-pic -fno-pie`;
  keep `-mno-sse/mmx`. `nasm -felf64`, `ld -melf_x86_64`. Switch to an explicit
  source list (`PORT64_CSOURCES`) that grows per milestone, so every commit
  builds and boots exactly what is ported. New targets: `boot.img` (Limine UEFI
  image via `build/mkboot.sh`), `run` (QEMU + OVMF), `test` (headless boot-smoke).
- `include/limine.h`: vendored Limine protocol header.
- `build/linker.ld`: rewritten for the Limine higher-half layout — link at
  `0xFFFFFFFF80000000`, `PHDRS` per segment, keep the `.limine_requests`
  section. Entry point is `kmain`.
- `build/limine.conf`, `build/mkboot.sh`: boot config and the sudo-free
  (`mtools`) UEFI image builder.
- `src/kernel.c`: Limine base-revision tag + memmap/HHDM requests; a minimal
  `kmain(void)` that proves long mode and the protocol, then halts. The original
  32-bit boot body is preserved under `#if 0` and re-enabled milestone by
  milestone. The old `src/loader.asm` trampoline is removed (Limine replaces it).

Limine hands us HHDM at `0xffff800000000000` and a 35-entry memory map, so the
higher-half environment and the protocol are both confirmed working.

## Milestone 1 — 4-level paging + memory subsystem  ✅ DONE

**Checkpoint:** the frame allocator, kernel heap, and a fresh 4-level mapping all
work. Validated by `make test` (marker `memory subsystem OK`): 4-level
`map_page`/`physical_address` round-trip a value, `kmalloc` returns writable
memory, and `frame_alloc` yields distinct frames.

- `include/paging.h`: replaced the 2-level structures with a single 64-bit
  entry type and PML4/PDPT/PD/PT indexing (`PML4_INDEX(x)=(x>>39)&0x1FF`, etc.).
  A `page_directory` is now just the physical address of its PML4 — under the
  HHDM there is no need for a software shadow of the tables.
- `src/paging.c`: rewritten for the HHDM model — `map_page`, `physical_address`
  and the `user_access_ok`/`user_string_ok` guards walk four levels and edit the
  live tables directly through `phys_to_virt`. The kernel heap lives in a private
  higher-half window (PML4 slot 384). Fork/copy-on-write, the page-fault handler
  and teardown are intentionally deferred (they need the task/exception
  subsystems) and return in milestones 2–3.
- `src/frames.c`, `include/frames.h`: 64-bit physical addresses; initialised
  over the largest usable region from the Limine memory map, with the bitmap and
  refcounts stored in that region via the HHDM.
- `src/bitset.c`: `bitset_search` ported from the 32-bit asm to portable C.
- `src/utils.c`: portable C `memset`/`memcpy` (were in the 32-bit optutils.asm).
- `include/panic.h`: port-era serial `kernel_panic` (the VGA/scrn one returns
  once scrn is ported).
- `include/types.h`: add `uintptr`/`uint64` aliases.

**Risk (realised):** largest code volume; the HHDM model actually *simplified*
page-table editing versus the 32-bit temporary-mapping dance.

## Milestone 2 — 64-bit IDT and interrupt/exception entry  ✅ DONE

**Checkpoint:** a breakpoint trap is caught and returned from, and the timer IRQ
fires and returns cleanly. Validated by `make test` (marker `interrupts OK`):
`int3 handled=1`, `timer ticks=3`.

- `include/idt.h`: 16-byte 64-bit gate descriptor (offset split low/mid/high +
  `ist`), an `interrupt_frame` matching the stub push order, and the handler API.
- `src/isr.asm` (new): 48 entry stubs (exceptions 0-31, IRQs 32-47). Error-code
  vectors (8, 10-14, 17) leave the CPU error code in place; the rest push a dummy
  0, so the frame is uniform. A common trampoline saves all 15 GP registers,
  16-byte-aligns the stack, calls the C dispatcher, restores and `iretq`s. An
  `isr_stub_table` exports the stub addresses for the IDT.
- `src/idt.c` (new): builds the IDT, installs every stub with the running kernel
  code selector (read from CS rather than hard-coded), and `lidt`s.
- `src/interrupts.c` (new): the C dispatcher, 8259 PIC remap to 0x20-0x2F, PIT at
  ~100 Hz, a tick-counting timer handler, and a serial fault dump for unhandled
  exceptions. No `swapgs` yet — that arrives with user mode in milestone 3.
- `src/serial.c`: `serial_dec`/`serial_hex` promoted from kmain locals so the
  fault dump can share them.

The full `proc.h` register-struct rework and the rich VGA fault screen wait for
the task/scrn subsystems (milestone 3); `interrupt_frame` stands in for now.

## Milestone 3 — Software context switch  ✅ DONE

**Checkpoint:** multitasking works via software context switching (long mode has
no hardware task switching). Validated by `make test` (marker
`context switch OK`): three cooperative kernel threads round-robin and each
advances its own counter (`a=5 b=5 c=5`), proving the switch preserves and
restores each thread's stack and registers independently.

- `src/context.asm` (new): `context_switch(old_rsp, new_rsp)` — push the
  callee-saved registers, stash the outgoing `rsp`, load the incoming `rsp`,
  restore and `ret` into the incoming thread. This is the long-mode replacement
  for the 32-bit hardware-TSS `jmp far`.
- `src/sched.c`, `include/sched.h` (new): a minimal cooperative kernel-thread
  scheduler. `thread_create` builds an initial stack (six zeroed callee-saved
  slots + the entry address) so the first switch `ret`s straight into the thread;
  `yield` round-robins.

This delivers the core mechanism. Full user-mode processes — the per-CPU 64-bit
TSS (`rsp0`), per-process address spaces (`clone_directory`), fork and the ELF
loader — build on top of it in milestones 4-5.

## Milestone 4 — Syscall ABI widening

**Checkpoint:** kernel-side syscalls take 64-bit arguments.

- `src/syscalls.c`: keep `int 0x80`; change register reads
  `ebx/ecx/edx → rdi/rsi/rdx` (or same registers, widened). The `REQUIRE_USER_*`
  boundary guards work unchanged against the new `user_access_ok`.

## Milestone 5 — 64-bit userland and disk image

**Checkpoint:** `make run` boots to the shell.

- `src/elf.c`: parse ELF64 headers; the user link base moves to a low canonical
  address.
- `build/tasks/*`, `build/bootstrap/`: rewrite `syscall_wrappers.asm` for the
  64-bit convention; compile tasks 64-bit (`elf_x86_64`); widen the USTACK/KSTACK
  constants in `include/tasks.h`.
- `tests/run-qemu.sh`, `tests/boot-smoke.sh`: use `qemu-system-x86_64`. The
  `ktest.c` harness and the isa-debug-exit contract are unchanged; add
  long-mode-specific checks (a canonical high address is mapped; a 4-level walk
  resolves).

---

## Effort and risk summary

| Milestone      | Size | Main risk                                             |
| -------------- | ---- | ----------------------------------------------------- |
| 0 Trampoline   | M    | 32→64 handoff triple-faults silently.                 |
| 1 Paging       | L    | Largest code volume; higher-half addressing bugs.     |
| 2 IDT/entry    | M    | Manual register save/restore must match the structs.  |
| 3 Ctx switch   | L    | Biggest conceptual rewrite; touches fork + scheduler. |
| 4 Syscall ABI  | S    | Mechanical.                                           |
| 5 Userland     | M    | ELF64 plus rebuilding the whole `build/` tree.        |

Milestones 1 and 3 carry most of the effort. Everything here is known OS-dev
work with no research risk, and the existing QEMU test suite
(`make test`, `tests/boot-smoke.sh`) is the regression gate at every checkpoint.

## What this port deliberately does not do

- No SMP / multicore: that is a separate concurrency project whose first task is
  replacing `cli`-as-lock with real spinlocks while still single-core.
- No `syscall`/`sysret` instruction migration (kept `int 0x80`).
- No PML5 / 5-level paging.
