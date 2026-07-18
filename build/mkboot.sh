#!/usr/bin/env bash
# Build a bootable UEFI image for the kernel using Limine, entirely in
# userspace (mtools — no sudo, no loopback mount). The result is a raw FAT
# image that OVMF boots from the removable-media path /EFI/BOOT/BOOTX64.EFI.
set -euo pipefail

KERNEL="${1:-kernel.bin}"
OUT="${2:-boot.img}"
MODULE="${3:-}" # optional userland ELF loaded as a Limine module

if ! command -v limine >/dev/null 2>&1; then
    echo "error: limine not found (is the devenv shell active?)" >&2
    exit 127
fi
LIMINE_SHARE="$(dirname "$(dirname "$(command -v limine)")")/share/limine"
EFI="$LIMINE_SHARE/BOOTX64.EFI"
if [ ! -f "$EFI" ]; then
    echo "error: $EFI missing" >&2
    exit 1
fi

rm -f "$OUT"
truncate -s 64M "$OUT"
mformat -i "$OUT" -F ::                       # FAT32 across the whole image
mmd -i "$OUT" ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
mcopy -i "$OUT" "$EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$OUT" "$KERNEL" ::/boot/kernel.bin
mcopy -i "$OUT" build/limine.conf ::/boot/limine/limine.conf
if [ -n "$MODULE" ]; then
    mcopy -i "$OUT" "$MODULE" ::/boot/hello.elf
fi

echo "wrote $OUT (kernel: $KERNEL${MODULE:+, module: $MODULE})"
