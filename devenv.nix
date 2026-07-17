{ pkgs, ... }:

{
  # Development environment for juampi-os.
  # Enter it with `devenv shell` (or automatically via direnv + .envrc).
  packages = [
    pkgs.gnumake
    pkgs.nasm
    pkgs.gcc_multi # gcc with 32-bit multilib support (the kernel builds -m32)
    pkgs.binutils # ld with elf_i386 support
    pkgs.e2tools # e2cp, used to populate the GRUB floppy image
    pkgs.util-linux # losetup + mkfs.minix, needed by build/build_image.sh
    pkgs.bochs # emulator + bximage (replaces ./install_bochs.sh)
    pkgs.clang-tools # clang-format for `make format` / `make lint`
  ];

  # Point the build at the Nix-provided Bochs instead of a locally built one,
  # so you don't need to run ./install_bochs.sh.
  env = {
    BOCHSDIR = "${pkgs.bochs}/bin";
    BXIMAGE = "${pkgs.bochs}/bin/bximage";
  };

  enterShell = ''
    echo "juampi-os dev shell"
    echo "  make kernel.bin   build just the kernel (no sudo)"
    echo "  make              full build (the disk-image step still needs sudo)"
    echo "  make run          build everything and boot in Bochs"
    echo "  make format|lint  run clang-format"
    echo
    echo "Note: build_image.sh mounts a loopback device and needs real sudo,"
    echo "which a pure Nix shell cannot provide. Run 'make image' on a host"
    echo "where you have sudo, or build only the kernel with 'make kernel.bin'."
  '';
}
