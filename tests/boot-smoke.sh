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

# Optionally attach a data disk (ext2) as the primary IDE slave, so tests can
# exercise the disk/fs libraries and run()-from-disk. Enabled by setting DISK.
DISK="${DISK:-}"
disk_args=()
if [ -n "$DISK" ] && [ -f "$DISK" ]; then
    disk_args=(-drive "file=$DISK,format=raw,if=none,id=juampidisk" \
        -device ide-hd,drive=juampidisk,bus=ide.0,unit=1)
fi

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
# before the line is delivered), then keep the pipe open until QEMU is stopped by
# timeout. Holding it open matters: boot to the shell can take longer than the
# leading delay (SMP bring-up + per-core Lua init, especially under TCG on CI),
# and if the writer exits first, QEMU sees EOF and the queued input is dropped
# before the shell ever reads it.
{
    sleep 8
    printf '%s\r' "$INPUT"
    sleep 40
} | timeout 45 "$QEMU" -bios "$ovmf_copy" \
    -drive file="$IMG",format=raw -m 512 \
    -smp "${QEMU_SMP:-4}" \
    "${disk_args[@]}" \
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
