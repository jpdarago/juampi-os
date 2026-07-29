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

- **newlib** (built from source for `x86_64-elf`, `--disable-newlib-supplied-
  syscalls`, so it calls our stubs). Bootstrap build: `build/hosted/build-newlib.sh`
  runs newlib's own autotools build with the nix cross toolchain
  (`pkgsCross.x86_64-embedded`) into the gitignored `.newlib/`. A curated,
  vendored subset compiled per-file by our Makefile (like BearSSL/Lua/uACPI) is
  the planned follow-up; the autotools build is the get-it-working step.
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

## Building and running (bootstrap phase)

```sh
# One-time: build newlib for x86_64-elf into .newlib/
nix shell nixpkgs#pkgsCross.x86_64-embedded.buildPackages.gcc \
          nixpkgs#pkgsCross.x86_64-embedded.buildPackages.binutils \
  --command bash build/hosted/build-newlib.sh

# Link a program against newlib + our crt0 + stubs -> a ring-0 ELF at 0x400000
nix shell nixpkgs#pkgsCross.x86_64-embedded.buildPackages.gcc \
          nixpkgs#pkgsCross.x86_64-embedded.buildPackages.binutils \
  --command bash build/hosted/build-prog.sh build/hosted/hello.c build/disk/chello.elf

make disk.img          # ship it on the ext2 disk
# then, in the shell:  run("chello.elf")
```

`build/hosted/hello.c` (printf/snprintf/malloc) and `filetest.c` (fopen/fread/
fwrite over ext2) are the worked examples; both are verified in QEMU.

## Known limits / follow-ups

- **Not yet in `make`**: building hosted programs needs the cross toolchain,
  which isn't in devenv. The planned vendored-subset step compiles newlib and
  programs with the host GCC (which already emits x86_64 ELF), folding it into
  the normal build and `make test`.
- This newlib's `printf` mishandles the **`%z`** length modifier — use `%u`/`%lu`.
- No real process model: `fork`/`exec`/signals are `ENOSYS` stubs; `getpid` is 1.
- Stdout is line-buffered via the console; there is no `stdin` line editing beyond
  the console's own.
