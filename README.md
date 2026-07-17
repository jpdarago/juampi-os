juampi-os
=========

My kernel — the final project for *Organización del Computador II* (UBA - FCEyN).

Features
--------

* 32 bits.
* Multitasking with a scheduler.
* ATA hard disk via PIO.
* VFS and a Minix filesystem.
* ELF32 loader.
* Mini libc.
* Memory allocator.
* Copy on write.

Dependencies
------------

To install the dependencies, run:

    sudo apt-get install nasm e2tools gcc-multilib clang-format qemu-system-x86

`clang-format` is only needed for `make format` / `make lint`; `qemu-system-x86`
provides the emulator used by `make run` and `make test`.

Building
--------

You need `sudo` to assemble the Minix disk image. This should go away in the
near future.

**WARNING:** THE DISK IMAGE IS BUILT BY ATTACHING IT TO A LOOP DEVICE AND
MOUNTING IT. REVIEW `build/build_image.sh` BEFORE RUNNING `make run` SO YOU
DON'T BREAK ANYTHING.

To build and run:

    make && make run

Useful targets (see `make help` for the full list):

| Target            | Description                                              |
|-------------------|----------------------------------------------------------|
| `make`            | Build the kernel, userland and bootable floppy image     |
| `make kernel.bin` | Build just the kernel binary (no disk image, no sudo)    |
| `make image`      | Build the Minix hard-disk image (needs sudo)             |
| `make run`        | Build and boot the OS in a QEMU window (override `QEMU_DISPLAY`) |
| `make format`     | Reformat all C sources/headers with clang-format         |
| `make lint`       | Check formatting without modifying files (used by CI)    |
| `make clean`      | Remove all build artifacts                               |

For anything else, look at the Makefile.

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
