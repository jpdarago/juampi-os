#!/usr/bin/env bash
# Build newlib (libc.a + libm.a) for x86_64-elf from source, with its own
# autotools build, into .newlib/. We provide the syscalls ourselves
# (--disable-newlib-supplied-syscalls -> libgloss stubs in syscalls.c), so this
# is a freestanding libc with no default OS assumptions. A one-time bootstrap:
# once the whole hosted-program chain works, a curated subset gets vendored into
# the tree and compiled per-file like the other third-party libs.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/.newlib/newlib-4.5.0.20241231"
BUILD="$ROOT/.newlib/build"
PREFIX="$ROOT/.newlib/prefix"

[ -d "$SRC" ] || { echo "newlib source not unpacked at $SRC" >&2; exit 1; }

rm -rf "$BUILD" "$PREFIX"
mkdir -p "$BUILD"
cd "$BUILD"

"$SRC/configure" \
    --target=x86_64-elf \
    --prefix="$PREFIX" \
    --disable-newlib-supplied-syscalls \
    --disable-multilib \
    --disable-newlib-mb

make -j"$(nproc)"
make install

echo "=== artifacts ==="
find "$PREFIX" -name 'libc.a' -o -name 'libm.a' -o -name 'crt0.o' 2>/dev/null
echo "=== headers at: $PREFIX/x86_64-elf/include ==="
ls "$PREFIX/x86_64-elf/include" | head
