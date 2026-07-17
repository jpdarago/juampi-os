Code Review — juampi-os
=======================

Scope: a full pass over the kernel (`src/`, `include/`) and userland
(`build/tasks/`, `build/bootstrap/`), grouped into four subsystems: memory,
filesystem/block-I/O, process/scheduler/syscall/ELF, and drivers/init/userland.

This environment has no C/asm toolchain installed, so nothing here was
compile- or run-tested. Findings were confirmed by reading the exact source
lines before being acted on. Status legend:

- **FIXED** — a fix was applied in this branch.
- **OPEN** — real (or plausible) issue left unfixed, with rationale.
- **NOT A BUG** — automated review flagged it, verification showed it's fine.

--------------------------------------------------------------------------

## Fixed

| # | File:line | Sev | Issue | Fix applied |
|---|-----------|-----|-------|-------------|
| 1 | `src/fs_minix.c:71` | High | `write_data_entry` called `buffered_read` instead of write — indirect block pointers were never persisted (filesystem corruption for files large enough to need indirect blocks). | Now calls `buffered_write`; added an `inonum` parameter, passed `ino->inode_number` from the caller (`inode_offset_position`). |
| 2 | `src/fs_minix.c:477` | High | `minix_mkdir` wrote both the `.` and `..` entries at offset 0, so `..` overwrote `.`. | Second entry now written at offset `sizeof(ode)`. |
| 3 | `src/tasks.c:253` | High | Fork copied `regs->ebx` into the child's EDI (copy/paste bug). | Now `new_tss->edi = regs->edi`. |
| 4 | `src/elf.c:45` | Med | `elf_get_segment` used `index > ph_entry_count`, allowing a read one past `program_header[]`. | Changed to `>=`. |
| 5 | `src/scrn.c:20` | Med | `scrn_valid_pos` used `<=`, treating row==`VIDEO_HEIGHT` / col==`VIDEO_WIDTH` as valid (out of range). | Changed to `<`. |
| 6 | `src/buffer_cache.c:137` | Med | `mark_dirty` allocated and linked a new `dirty_ln` on every write to an already-dirty buffer → leaked nodes + double-free risk on flush. | Early-return when `b->dlist_ptr != NULL`; removed the redundant `b->dirty = true` in `buffered_write`. |
| 7 | `src/tasks.c:504` | Med | `next_task` dereferenced `search_available_task()` without a NULL check. | Added `if(p == NULL) return NULL;` (matches the function's existing "no task" contract). |
| 8 | `src/memory.c:53` | Med | `kmem_alloc(int size)` only guarded `size == 0`; a negative size passed through and could overflow `size + sizeof(header)`. | Guard is now `size <= 0`. (Left the `int` signature to avoid a wide API change — see Open #4.) |
| 9 | `src/handlers.c:31` | Low | `scrn_printf("...%u...", irq_code, irq_code)` — two args for one specifier; also a Spanish typo. | Now `scrn_printf("Unknown interrupt %u", irq_code)`. |
| 10 | `src/syscalls.c:166` | Low | Dead `if(syscalls[code] == NULL)` branch (already guarded on the line above). | Removed the dead branch. |

--------------------------------------------------------------------------

## Open (not fixed) — with rationale

1. **User pointers are not validated at the syscall boundary.**
   `src/syscalls.c` and the `do_*` handlers in `src/tasks.c` pass user-supplied
   pointers/lengths (from saved registers) straight into kernel code and
   dereference them — unbounded `strcpy` in `do_get_cwd`/`do_set_cwd`, unbounded
   `strcat` into a fixed 129-byte buffer in `process_filename`, unvalidated
   `argv` walks in `count_arguments`/`push_into_user_stack`. This is the
   highest-value hardening item, but it's an **architectural change** (needs a
   defined user/kernel address split, an `is_user_pointer(ptr,len)` gate, and
   bounded string copies) that must be tested against a real boot — too risky to
   apply blind. Recommend doing this next, with a build available.

2. **ELF loader trusts untrusted header offsets** (`src/elf.c:24,51`).
   `ph_offset`/`ph->offset` are added to the image base with no bounds check.
   A proper fix requires threading the loaded image size through
   `elf_read_exec` (which currently takes only `void* image`), so it's an API
   change; deferred. In practice the images are kernel-loaded GRUB modules, so
   this is hardening rather than an active crash.

3. **Inode reference leaks on early-return error paths** (`src/fs.c`,
   e.g. `process_path` returning on a non-directory component without
   `release_inode`). Localized but needs care to place the release correctly on
   every path; left for a pass with tests.

4. **`kmem_alloc` still takes a signed `int size`.** Changing it to unsigned
   ripples through `kmalloc` and all callers; deferred. The `size <= 0` guard
   (Fixed #8) covers the immediate negative-size hazard.

5. **PID allocation never wraps/among-checks** (`src/tasks.c` `last_pid`).
   Design-level; not a crash today.

6. **Defensive NULL guards in paging** (`do_copy_on_write`, `clear_page_entry`
   dereference `tables_virtual[pdi]`). For a legitimate present-page fault the
   table exists, so these are not live bugs; adding `fail_if` guards per
   STYLE.md §8 would be a reasonable hardening follow-up.

7. **Verify** the PIT reload byte order (`src/timer.c:12`) and that the CMOS
   `update_in_progress()` busy-wait (`src/cmos.c:42`) reads through `volatile`
   port I/O. Both need hardware/emulator observation to confirm.

--------------------------------------------------------------------------

## Not a bug (automated findings rejected after verification)

- **`memcpy`/`memset` "copy backwards"** (`build/tasks/utils.c:34-48`): the
  `while(bytes-- > 0)` loop copies indices `N-1..0` — a *complete* copy in
  reverse order, which is correct for non-overlapping memory (all `memcpy`
  promises). Not a bug.
- **TTY backspace `read -= 2`** (`src/tty.c:20`): correct. After
  `data[read++] = '\b'`, `read` points past the just-stored backspace; `-= 2`
  removes both that `\b` and the preceding character, leaving `read` at the
  right place.
- **`bitset_init` `memset(start, 0, size/8)` undersizing** (`src/bitset.c:24`):
  the top partial dword is fully **assigned** at line 26
  (`~((1 << (size % 32)) - 1)`), which overwrites any bytes the `size/8` memset
  left uncleared, and all lower dwords are fully covered. Correct in all cases.
- **Missing TLB flush in `copy_frame`** (`src/copy_frame.asm`): paging is only
  toggled off/on with CR3 and the mappings unchanged, so the existing TLB
  entries stay valid; no flush is required.
- **"`do_rmdir` calls `mkdir`"** (`src/fs.c:244`): the function there is
  `do_mkdir` (it checks and calls the `mkdir` op) — a misidentification, not a
  bug.

--------------------------------------------------------------------------

## Recommendations

1. **Syscall-boundary hardening (Open #1)** is the single highest-value next
   step; it also subsumes the ELF-loader offset check (Open #2).
2. Add the `fail_if`/`fail_unless` assertions STYLE.md already mandates to the
   allocator, bitset, and filesystem — several fixed bugs would have tripped an
   assertion immediately.
3. Bring up the build with the new `devenv.nix` and run the kernel under Bochs
   to validate the filesystem fixes (#1, #2, #6) against a live Minix image.
