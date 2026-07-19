#!/usr/bin/env bash
# Build a bootable UEFI image for the kernel using Limine, entirely in
# userspace (mtools — no sudo, no loopback mount). The result is a raw FAT
# image that OVMF boots from the removable-media path /EFI/BOOT/BOOTX64.EFI.
set -euo pipefail

KERNEL="${1:-kernel.bin}"
OUT="${2:-boot.img}"
shift 2 2>/dev/null || true
SCRIPTS=("$@") # Lua scripts, each loaded as a Limine module under /boot/scripts

# Locate Limine's UEFI loader: either from LIMINE_DIR (a directory containing
# BOOTX64.EFI, e.g. a checkout of the limine binary branch in CI) or from the
# installed limine package (devenv shell).
if [ -n "${LIMINE_DIR:-}" ]; then
    LIMINE_SHARE="$LIMINE_DIR"
elif command -v limine >/dev/null 2>&1; then
    LIMINE_SHARE="$(dirname "$(dirname "$(command -v limine)")")/share/limine"
else
    echo "error: limine not found (activate the devenv shell or set LIMINE_DIR)" >&2
    exit 127
fi
EFI="$LIMINE_SHARE/BOOTX64.EFI"
if [ ! -f "$EFI" ]; then
    echo "error: $EFI missing" >&2
    exit 1
fi

rm -f "$OUT"
truncate -s 64M "$OUT"
mformat -i "$OUT" -F ::                       # FAT32 across the whole image
mmd -i "$OUT" ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine ::/boot/scripts
mcopy -i "$OUT" "$EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$OUT" "$KERNEL" ::/boot/kernel.bin
mcopy -i "$OUT" build/limine.conf ::/boot/limine/limine.conf
for s in "${SCRIPTS[@]}"; do
    [ -n "$s" ] && mcopy -i "$OUT" "$s" "::/boot/scripts/$(basename "$s")"
done

echo "wrote $OUT (kernel: $KERNEL, scripts: ${SCRIPTS[*]:-none})"
