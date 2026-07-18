#!/usr/bin/env bash
set -uo pipefail

# Boot the Limine UEFI image (boot.img) in QEMU under OVMF, drive the serial
# shell with a scripted line of input, and assert the expected response comes
# back. This proves the kernel booted all the way through its self-tests into an
# interactive shell that reads input and evaluates it.

QEMU="${QEMU:-qemu-system-x86_64}"
# Input fed to the shell over serial, and the marker its evaluation must print.
INPUT="${INPUT:-print[[SHELL_ALIVE_9271]]}"
MARKER="${MARKER:-SHELL_ALIVE_9271}"
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

# Feed the shell input on stdin (a leading delay lets the kernel finish booting
# before the line is delivered), then let timeout stop the forever-looping shell.
{
    sleep 6
    printf '%s\r' "$INPUT"
    sleep 4
} | timeout 30 "$QEMU" -bios "$ovmf_copy" \
    -drive file="$IMG",format=raw -m 512 \
    -display none -serial stdio -no-reboot >"$out" 2>&1

echo "--- serial output ---"
cat "$out"
echo "---------------------"
rc=1
if grep -q "$MARKER" "$out"; then
    echo "PASS: shell booted and evaluated input ('$MARKER')"
    rc=0
else
    echo "FAIL: marker '$MARKER' not found (boot or shell failed)" >&2
fi
rm -f "$out" "$ovmf_copy"
exit $rc
