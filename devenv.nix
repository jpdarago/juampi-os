{ pkgs, ... }:

{
  # Development environment for juampi-os.
  # Enter it with `devenv shell` (or automatically via direnv + .envrc).
  #
  # The default `make` builds with the host GCC in 32-bit mode (gcc_multi,
  # Linux only). For a cross build — required on macOS, and `make CROSS=i686-elf-`
  # elsewhere — you need an i686-elf toolchain. In Nix that is roughly:
  #     pkgs.pkgsCross.i686-embedded.buildPackages.gcc
  #     pkgs.pkgsCross.i686-embedded.buildPackages.binutils
  # (verify the attribute name against your nixpkgs pin before adding it), or on
  # macOS `brew install i686-elf-gcc i686-elf-binutils`.
  packages = [
    pkgs.gnumake
    pkgs.nasm
    pkgs.qemu_kvm # host-only QEMU with a display (qemu-system-i386), run + tests
    pkgs.clang-tools # clang-format for `make format` / `make lint`
    pkgs.e2fsprogs # mke2fs / debugfs / e2fsck for the ext2 disk image
    pkgs.e2tools # e2cp / e2mkdir to populate the ext2 image (and the floppy)
  ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
    pkgs.gcc_multi # host gcc with 32-bit multilib (the Makefile's default)
    pkgs.binutils # host ld with elf_i386 support
  ];

  enterShell = ''
    echo "juampi-os dev shell"
    echo "  make              full build (no sudo — userspace image builder)"
    echo "  make run          build and run in QEMU"
    echo "  make test         run the in-kernel tests under QEMU"
    echo "  make format|lint  run clang-format"
    echo
    echo "macOS / cross build: install an i686-elf toolchain and use"
    echo "  make CROSS=i686-elf- run   (the ext2 image build is sudo-free and"
    echo "  cross-platform, so the full OS builds and runs on macOS too)."
  '';
}
