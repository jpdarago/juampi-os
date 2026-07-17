#!/usr/bin/env bash
set -uo pipefail

# Boot the full OS in QEMU (kernel + init module + Minix disk image) and assert
# it boots through the filesystem mount into userland. Requires kernel.bin,
# build/bootstrap/init and hdd.img to be built already, plus qemu.
#
# The check is the serial marker printed right before jumping to the initial
# task: reaching it means the superblock read, the Minix mount and the init
# module load all succeeded. If the filesystem were unreadable the kernel would
# panic earlier and the marker would never appear.

QEMU="${QEMU:-qemu-system-i386}"
MARKER="entering userland"
out="$(mktemp)"

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "error: $QEMU not found" >&2
    exit 127
fi

timeout 20 "$QEMU" -kernel kernel.bin -initrd build/bootstrap/init \
    -drive file=hdd.img,format=raw,if=ide -m 128 \
    -display none -serial stdio -no-reboot >"$out" 2>&1 &
qpid=$!

# The shell then idles waiting for keyboard input, so QEMU won't exit on its
# own; give it time to boot, then stop it.
sleep 8
kill "$qpid" 2>/dev/null
wait "$qpid" 2>/dev/null

echo "--- serial output ---"
cat "$out"
echo "---------------------"
if grep -q "$MARKER" "$out"; then
    echo "PASS: kernel mounted the filesystem and reached userland"
    rm -f "$out"
    exit 0
else
    echo "FAIL: did not reach userland (filesystem mount or boot failed)" >&2
    rm -f "$out"
    exit 1
fi
