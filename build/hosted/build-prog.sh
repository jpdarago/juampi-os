#!/usr/bin/env bash
# Compile + link one hosted C program against our freshly-built newlib and our
# crt0 + libgloss stubs, producing a static ring-0 ELF (entry _start, loaded at
# 0x400000). Run inside a nix shell that provides x86_64-elf-gcc.
#   build-prog.sh <source.c> <out.elf>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/build/hosted"
PREFIX="$ROOT/.newlib/prefix"
NLINC="$PREFIX/x86_64-elf/include"
NLLIB="$PREFIX/x86_64-elf/lib"

SRC="$1"
OUT="$2"
CC=x86_64-elf-gcc
CFLAGS="-O2 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -Wall"
LIBGCC="$($CC -print-libgcc-file-name)"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

$CC $CFLAGS -c "$HERE/crt0.S"    -o "$tmp/crt0.o"
$CC $CFLAGS -I"$NLINC" -c "$HERE/syscalls.c" -o "$tmp/syscalls.o"
$CC $CFLAGS -I"$NLINC" -c "$SRC" -o "$tmp/prog.o"

# -nostdlib: use OUR newlib (-L$NLLIB -lc -lm) + the compiler's libgcc, not the
# toolchain's bundled libc. -no-pie/static, text at 0x400000 (low canonical).
$CC -nostdlib -static -no-pie \
    -Wl,-Ttext-segment=0x400000 \
    "$tmp/crt0.o" "$tmp/prog.o" "$tmp/syscalls.o" \
    -L"$NLLIB" -lc -lm "$LIBGCC" \
    -o "$OUT"

echo "=== $OUT ==="
x86_64-elf-readelf -h "$OUT" | grep -E "Type|Entry|Machine"
x86_64-elf-nm "$OUT" | grep -E " T (_start|main)$" || true
