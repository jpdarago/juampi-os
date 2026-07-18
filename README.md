juampi-os
=========

My kernel — originally the final project for *Organización del Computador II*
(UBA - FCEyN), since ported to x86-64.

Features
--------

* 64-bit (x86-64 long mode), booted by [Limine](https://github.com/limine-bootloader/limine).
* Higher-half kernel with 4-level paging over Limine's direct map (HHDM).
* Interrupts: 64-bit IDT, PIC + PIT timer, serial fault dumps.
* Software context switching with cooperative kernel threads.
* Ring 3 user mode with an `int 0x80` syscall ABI and validated user pointers.
* ELF64 loader: runs a real user program shipped as a Limine module.

The port is documented milestone by milestone in `docs/x86-64-port.md`. The
original 32-bit kernel — multitasking scheduler, ATA PIO disk, read/write ext2
VFS, copy-on-write fork — lives on the `main` branch history; re-enabling those
subsystems on the 64-bit base is the next phase of work.

Building and running
--------------------

The repo ships a Nix/devenv environment (`devenv shell`, or automatic with
direnv) providing the whole toolchain: host GCC, nasm, QEMU, Limine, mtools and
clang-format. Then:

    make && make run

| Target            | Description                                            |
|-------------------|--------------------------------------------------------|
| `make`            | Build the kernel and the bootable UEFI image           |
| `make kernel.bin` | Build just the kernel binary                           |
| `make run`        | Boot the image in QEMU under OVMF (`QEMU_DISPLAY=...`) |
| `make test`       | Headless boot-smoke test (serial marker)               |
| `make format`     | Reformat all C sources/headers with clang-format       |
| `make lint`       | Check formatting without modifying files (used by CI)  |
| `make clean`      | Remove all build artifacts                             |

The boot image is a plain FAT/UEFI image built entirely in userspace with
`mtools` — no `sudo`, no loopback mounts. Without Nix, install
`gcc nasm qemu-system-x86 ovmf mtools clang-format`, fetch Limine's binary
branch, and point the build at them (see `.github/workflows/ci.yml` for the
exact recipe).

Testing
-------

`make test` boots the full image headless under OVMF and asserts, via the
serial log, that the kernel came up through every subsystem self-test — memory,
interrupts, context switching — and into ELF64 userland (the user program's
`write` syscall output is the final marker). CI runs the same test.

Documentation
-------------

* `docs/x86-64-port.md` — the x86-64 port, milestone by milestone.
* `informe/` — the original project report (in Spanish); `make` inside that
  folder generates the PDF.

TODOs
------

* Re-enable the OS services on the 64-bit base: processes/fork, the ext2 VFS,
  the interactive shell.
* Multicore (via Limine's MP request; needs real locking first).
* Port a libc (musl).
* Preemptible kernel, DMA disk I/O, framebuffer console.

Acknowledgements
---------------

* See the acknowledgements in the report.
* OSDev Wiki: <http://osdev.org>
* James Molloy's kernel development tutorials: <http://jamesmolloy.co.uk/tutorial_html>
