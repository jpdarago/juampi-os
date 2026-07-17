# Porting juampi-os to x86-64

This document scopes the migration of the kernel from 32-bit protected mode to
64-bit long mode. It is organised as **six milestones**, each ending at a
bootable checkpoint validated under QEMU, so the tree is never left in a
long-lived uncompilable state. Work happens on the `feature/x86-64` branch; each
milestone is one commit.

## Guiding decisions

- **Address-space layout: higher-half.** The kernel is linked at
  `0xFFFFFFFF80000000` (the top 2 GB canonical region) and loaded physically at
  1 MB. User space lives in the low canonical half. 64-bit is the natural time
  to make this change.
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

## Milestone 0 — Toolchain + long-mode trampoline

**Checkpoint:** the kernel prints from 64-bit code.

- `Makefile`, `build/tasks/Makefile`: drop `-m32`; add `-mcmodel=kernel`,
  `-mno-red-zone`, `-fno-pic -fno-pie`; keep `-mno-sse/mmx`. `nasm -felf64`,
  `ld -melf_x86_64`.
- `devenv.nix`: ensure `qemu-system-x86_64` is available (it is).
- `src/loader.asm`: keep the Multiboot-1 header (GRUB still enters in 32-bit
  protected mode). Add a 32-bit stub that builds temporary PML4/PDPT/PD tables
  (identity-map + higher-half alias the first 1 GB with 2 MB pages), sets
  `CR4.PAE`, `EFER.LME` (MSR `0xC0000080`), `CR0.PG`, loads a 64-bit GDT, and
  far-jumps into a 64-bit code segment that sets `rsp` and calls `kmain`.
- `build/linker.ld`: link high (`. = 0xFFFFFFFF80100000`) while loading at 1 MB
  (`AT(...)`).
- `src/gdt.c`, `include/gdt.h`, `src/desc.asm`: a minimal 64-bit GDT (null,
  kernel code `L=1`, kernel data, user code, user data). `gdt_desc` base widens
  to 64-bit.

**Risk:** the 32→64 handoff triple-faults silently. Debug with
`qemu -d int,cpu_reset -no-reboot`.

## Milestone 1 — 4-level paging

**Checkpoint:** kernel heap and frame allocator work under the new page tables.

- `include/paging.h`: replace the 2-level structures with PML4/PDPT/PD/PT
  (512 × 8-byte entries). New index macros: `PML4(x)=(x>>39)&0x1FF`,
  `PDPT(x)=(x>>30)&0x1FF`, `PD(x)=(x>>21)&0x1FF`, `PT(x)=(x>>12)&0x1FF`.
  `page_entry` becomes a 64-bit entry (present/rw/user/nx/frame:40).
- `src/paging.c`: rewrite `physical_address`, `map_page`, `create_table_entry`,
  `clone_directory`, and `user_access_ok`/`user_string_ok` (walk four levels;
  the syscall-hardening logic ports directly, just deeper). Heap window
  constants move into canonical high memory.
- `include/memory.h`, `src/frames.c`: addresses widen to 64-bit; the frame
  bitmap can cover more than 4 GB.

**Risk:** largest code volume; higher-half addressing bugs.

## Milestone 2 — 64-bit IDT and interrupt/exception entry

**Checkpoint:** exceptions and the timer IRQ fire and return cleanly.

- `include/idt.h`, `src/idt.c`: `idt_entry` becomes 16 bytes — offset split into
  low/mid/high (bits 0-15 / 16-31 / 32-63) plus the `ist` field. Descriptor base
  widens.
- `src/interrupts_entry.asm`, `src/exception_entry.asm`,
  `src/syscalls_entry.asm`, `include/mode_switch.inc`: `pushad`/`popad` do not
  exist in 64-bit — push/pop all 15 GP registers manually. `iretd → iretq`. The
  long-mode interrupt frame always pushes SS:RSP. Most of the ds/es/fs/gs
  juggling disappears (long mode ignores those segment bases); it collapses to a
  `swapgs` at kernel entry/exit for the per-CPU GS base.
- `include/proc.h`: `gen_regs` → `rax…r15`; `int_trace` → `rip, cs, rflags,
  rsp, ss`; control-register trace widens. Ripples into every handler that reads
  registers (page-fault handler, syscall dispatch).

## Milestone 3 — Software context switch

**Checkpoint:** multitasking works again.

- `include/tss.h`: replace the 32-bit TSS with the 64-bit TSS (`rsp0/1/2`, 7 IST
  entries, iomap). One TSS **per CPU**, not per task.
- `src/task_switch.asm`: new software switch — save callee-saved registers, save
  the old `rsp` into the old task, load the new task's `rsp`, switch `cr3`,
  restore, `ret`.
- `src/tasks.c`: replace `init_tss`/`set_tss_gregs`/`get_contiguous_tss`/
  `new_tss_space` with "build an initial kernel-stack frame" for a new task;
  store a saved `rsp` per process; set the CPU TSS's `rsp0` to the task's kernel
  stack on switch.
- `src/proc.c`, `include/proc.h`: `process_info` holds a saved kernel `rsp`
  instead of a `tss_selector`; drop per-process `gdt_add_tss`/`gdt_remove_tss`.
- `src/fork.asm`: rewrite around the new frame layout and 64-bit ABI.

**Risk:** biggest conceptual rewrite; touches fork and the scheduler.

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
