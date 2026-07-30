# Hosted C programs (newlib) in ring 0

juampiOS can run ordinary ANSI-C programs — `main(argc, argv)`, stdio, `malloc`,
math, file I/O — with few or no source changes, by linking them against
**newlib** and running them in ring 0 (the same "lab" execution model as the
native benchmark ELFs). They reach kernel services through an `int 0x80` trap
rather than by linking against kernel symbols, so the program's libc is fully
self-contained.

This is the "path B" libc: **newlib**, not musl (musl assumes the Linux syscall
ABI and is hard to retarget); newlib is built for retargeting via a thin syscall
stub layer. See the discussion in the project history for the musl-vs-newlib
reasoning.

## Pieces

- **newlib** — a **curated subset vendored under `src/newlib/`** and compiled
  per-file by the host GCC in the Makefile (like BearSSL/Lua/uACPI). The subset
  (63 libc `.c` files: stdio, stdlib incl. `dtoa`/`mprec`, string, reent, ctype,
  locale, signal, errno) was chosen from a linker map of the demos, so nothing
  unused is carried. Subdirs are preserved so same-dir `"local.h"` includes
  resolve; the installed public headers sit in `src/newlib/include`. Compile
  flags that matter: `-D_LIBC` (disable newlib's fortify/ssp wrappers — the nix
  gcc wrapper's default `-D_FORTIFY_SOURCE=2` would otherwise pull them in),
  `-DMISSING_SYSCALL_NAMES` (reent layer calls our bare-named syscalls),
  `-DHAVE_MMAP=0` (sbrk-only malloc), `-nostdinc` + newlib headers only.
  Re-vendoring pipeline (only needed to change the subset): `build-newlib.sh`
  does newlib's autotools build for `x86_64-elf` (via the nix
  `pkgsCross.x86_64-embedded` toolchain) into the gitignored `.newlib/`, then
  `vendor-newlib.sh` copies the curated files into `src/newlib/`.
- **crt0** (`build/hosted/crt0.S`) — `_start(argc, argv)`: run `.init_array`
  constructors, call `main`, hand the result to `exit()` (which flushes stdio and
  traps out via the exit syscall). Also supplies empty `_init`/`_fini`.
- **libgloss stubs** (`build/hosted/syscalls.c`) — the bare POSIX hooks newlib's
  reentrant layer expects (`write`, `read`, `open`, `close`, `lseek`, `sbrk`,
  `fstat`, `isatty`, `gettimeofday`, `getpid`/`kill`/…). Each issues `int 0x80`
  (number in rax, args rdi/rsi/rdx, return in rax; negative = -errno). `fstat`
  and `isatty` are resolved locally so the kernel needn't know newlib's
  `struct stat` layout.
- **kernel syscall dispatcher** (`src/syscall.c`) — implements the trap over
  console I/O, a per-program `sbrk` heap, the RTC clock, and an ext2-backed file
  descriptor table. `int 0x80` works from ring 0 (a software interrupt through
  the DPL-3 gate); no privilege switch happens, but it's still a clean, stable
  boundary that would carry over to a future ring-3 port.
- **runner** — `hosted_run()` (in `src/syscall.c`) loads the ELF, gives it a
  heap + argv, jumps to `_start`, and returns `main`'s status. `exit` longjmps
  back (then re-enables interrupts — the int gate masked them, as with fault
  recovery). `run("prog.elf")` in `lua_run.c` routes here when the ELF defines
  `_start` (a hosted program); lab ELFs (entry `bench`) take the existing path.
  `elf64_symbol()` makes that distinction.

## Execution model

One hosted program runs at a time, synchronously, on the caller's stack, in ring
0 with no memory isolation (NX/SMEP/SMAP are off). A fault in the program is
caught by the shell's fault recovery, like any other ring-0 code. This is a
scriptable "run a C program" facility, not a protected multiprocess userland —
that would want the (currently dormant) ring-3 path plus preemptive scheduling.

Files are buffered whole in the kernel heap: reads serve from the buffer, writes
grow it, and `close` (or program teardown) flushes a dirty file to ext2 with
`ext2_write_file` — matching the raw-block Lua API's simple whole-file model.

## Building and running

Just `make` — the vendored newlib compiles with the host GCC, no cross toolchain
needed. The demo programs `build/hosted/chello.c` (printf/snprintf/malloc) and
`filetest.c` (fopen/fread/fwrite over ext2) build into Limine modules; run them
from the shell with `run("chello.elf")` / `run("filetest.elf")`. Both are in
`make test` (`HOSTED_OK`, `FILEIO_OK`).

To add a program: drop a `.c` in `build/hosted/`, add its name to `HOSTED_PROGS`
in the Makefile, and add a `module_path` line to `build/limine.conf`.

## Known limits / follow-ups

- This newlib's `printf` mishandles the **`%z`** length modifier — use `%u`/`%lu`.
- The vendored subset covers stdio/stdlib/string/math-printf; a program using
  more of libc (e.g. `<math.h>` functions, `qsort`, more of `<time.h>`) will hit
  an undefined reference — add the needed `.c` files to `src/newlib/` (re-run
  `vendor-newlib.sh` with the extra members) and rebuild.
- No real process model: `fork`/`exec`/signals are `ENOSYS` stubs; `getpid` is 1.
- Stdout is line-buffered via the console; there is no `stdin` line editing beyond
  the console's own.
