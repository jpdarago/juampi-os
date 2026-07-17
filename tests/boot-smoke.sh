#!/usr/bin/env bash
set -uo pipefail

# Boot the Limine UEFI image (boot.img) in QEMU under OVMF and assert the kernel
# reached 64-bit long mode and answered the Limine boot protocol. During the
# x86-64 port this is the per-milestone boot gate; the success marker moves
# further into boot as subsystems are ported (eventually "entering userland").

QEMU="${QEMU:-qemu-system-x86_64}"
MARKER="${MARKER:-Limine boot protocol OK}"
IMG="${IMG:-boot.img}"
out="$(mktemp)"

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "error: $QEMU not found" >&2
    exit 127
fi

# Resolve the OVMF firmware (env override, else nixpkgs). OVMF needs writable
# vars, so boot from a private copy.
OVMF_FD="${OVMF_FD:-$(nix build --no-link --print-out-paths nixpkgs#OVMF.fd 2>/dev/null)/FV/OVMF.fd}"
if [ ! -f "$OVMF_FD" ]; then
    echo "error: OVMF firmware not found at '$OVMF_FD'" >&2
    exit 127
fi
ovmf_copy="$(mktemp)"
cp "$OVMF_FD" "$ovmf_copy"
chmod +w "$ovmf_copy"

timeout 30 "$QEMU" -bios "$ovmf_copy" \
    -drive file="$IMG",format=raw -m 512 \
    -display none -serial stdio -no-reboot >"$out" 2>&1 &
qpid=$!

# The kernel halts after the boot proof, so QEMU won't exit on its own; give it
# time to boot, then stop it.
sleep 10
kill "$qpid" 2>/dev/null
wait "$qpid" 2>/dev/null

echo "--- serial output ---"
cat "$out"
echo "---------------------"
rc=1
if grep -q "$MARKER" "$out"; then
    echo "PASS: kernel booted into long mode ('$MARKER')"
    rc=0
else
    echo "FAIL: marker '$MARKER' not found (boot failed)" >&2
fi
rm -f "$out" "$ovmf_copy"
exit $rc
