{ pkgs, ... }:

{
  # Development environment for juampi-os (x86-64, booted by Limine).
  # Enter it with `devenv shell` (or automatically via direnv + .envrc).
  #
  # The kernel builds with the host GCC (the host is x86-64); OVMF firmware for
  # `make run` / `make test` is resolved from nixpkgs at recipe time.
  packages = [
    pkgs.gnumake
    pkgs.gcc # host gcc builds the freestanding 64-bit kernel
    pkgs.binutils # ld with elf_x86_64 support
    pkgs.qemu_kvm # qemu-system-x86_64 for run + tests
    pkgs.clang-tools # clang-format for `make format` / `make lint`
    pkgs.limine # 64-bit bootloader: boots the kernel straight into long mode
    pkgs.mtools # mformat/mcopy/mmd build the FAT boot image without sudo
    pkgs.socat # QMP scripting for the keyboard test (tests/kbd-smoke.sh)
  ];

  enterShell = ''
    echo "juampi-os dev shell (x86-64)"
    echo "  make              build the kernel + bootable Limine image"
    echo "  make run          boot it in QEMU (OVMF/UEFI)"
    echo "  make test         headless boot-smoke test"
    echo "  make format|lint  run clang-format"
  '';
}
