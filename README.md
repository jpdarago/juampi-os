juampi-os
=========

My kernel — the final project for *Organización del Computador II* (UBA - FCEyN).

Features
--------

* 32 bits.
* Multitasking with a scheduler.
* ATA hard disk via PIO.
* VFS with a read/write ext2 filesystem.
* ELF32 loader.
* Mini libc.
* Memory allocator.
* Copy on write.

Dependencies
------------

To install the dependencies, run:

    sudo apt-get install nasm e2tools e2fsprogs gcc-multilib clang-format qemu-system-x86

`clang-format` is only needed for `make format` / `make lint`; `qemu-system-x86`
provides the emulator used by `make run` and `make test`.

Building
--------

The ext2 disk image is built entirely in userspace — no `sudo`, no loopback
mount, and it works on macOS too. `mke2fs` creates the filesystem on a plain
file, `e2tools` (`e2cp`/`e2mkdir`) copies the tasks and docs in, and `debugfs`
adds the `/dev/tty` node. The kernel mounts it with a read/write ext2 driver
(`src/fs_ext2.c`).

To build and run:

    make && make run

Useful targets (see `make help` for the full list):

| Target            | Description                                              |
|-------------------|----------------------------------------------------------|
| `make`            | Build the kernel, userland and bootable floppy image     |
| `make kernel.bin` | Build just the kernel binary (no disk image, no sudo)    |
| `make image`      | Build the ext2 hard-disk image (no sudo)                 |
| `make run`        | Build and boot the OS in a QEMU window (override `QEMU_DISPLAY`) |
| `make format`     | Reformat all C sources/headers with clang-format         |
| `make lint`       | Check formatting without modifying files (used by CI)    |
| `make clean`      | Remove all build artifacts                               |

For anything else, look at the Makefile.

Cross-compiling (and macOS)
---------------------------

By default the build uses the host GCC in 32-bit mode (`-m32`), which needs
`gcc-multilib` and a Linux/ELF `ld` — so it does not work on macOS. Set `CROSS`
to a freestanding i686 cross-compiler prefix to build anywhere:

    make CROSS=i686-elf- kernel.bin
    make CROSS=i686-elf- test

Install the toolchain with `brew install i686-elf-gcc i686-elf-binutils` on
macOS (also `nasm qemu coreutils e2fsprogs e2tools`), or add one to `devenv.nix`
on Nix. CI builds and tests the cross path on macOS; the host path on Linux. The
ext2 disk image builds without sudo, so the full OS (`make CROSS=... run`) works
on macOS too.

Testing
-------

Integration tests run the kernel under QEMU headless. `make test` builds a
dedicated `kernel-test.bin` (compiled with `-DKTEST`) that boots, runs an
in-kernel test suite against the real allocator, frame allocator and MMU, and
then exits QEMU with a pass/fail code:

    make test

It needs `qemu-system-i386` (Debian/Ubuntu: `apt-get install qemu-system-x86`).
The kernel boots directly via `qemu -kernel` (no floppy/GRUB/sudo image), prints
results to the debug console, and signals the result through QEMU's
`isa-debug-exit` device. The tests live in `tests/ktest.c`; the runner is
`tests/run-qemu.sh`. Override the emulator or timeout if needed, e.g.
`make test TIMEOUT=60 QEMU=qemu-system-i386`. This job also runs in CI.

Host-side unit tests for the hardware-independent modules are a planned
follow-up (there are starting points under `workbench/`).

Documentation
-------------

The project report lives in the `informe/` folder (in Spanish). To generate the
PDF, run `make` from inside that folder.

TODOs
------

* 64 bits.
* Multicore.
* Implement SIGSEGV logic.
* Better shell (pipes, output redirection, more commands, etc.).
* Port a libc.
* Allow multiple processes to access the filesystem.
* Background jobs.
* Signals for processes (groups).
* Filesystem permissions.
* Preemptible kernel.
* Swapping to disk.
* Disk I/O via DMA.
* Optimize the algorithms.
* VGA or VESA driver.

The first two were implemented for the DeliriOS project
(<http://github.com/Izikiel/intel_multicore>). Check it out, it's cool.

Acknowledgements
---------------

* See the acknowledgements in the report.
* OSDev Wiki: <http://osdev.org>
* James Molloy's kernel development tutorials: <http://jamesmolloy.co.uk/tutorial_html>
