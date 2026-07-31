#!/usr/bin/env bash
set -uo pipefail

# Boot the Limine UEFI image (boot.img) in QEMU under OVMF, drive the serial
# shell with a scripted line of input, and assert the expected response comes
# back. This proves the kernel booted all the way through its self-tests into an
# interactive shell that reads input and evaluates it.

QEMU="${QEMU:-qemu-system-x86_64}"
# Machine realism: q35 (PCIe, like the XPS target) and, when KVM is available,
# the host CPU model (real CPUID/MSRs/PMU). CI has no /dev/kvm -> TCG fallback.
# Override QEMU_FLAGS to pin a specific machine/accel (e.g. legacy ATA tests).
QEMU_FLAGS="${QEMU_FLAGS:--machine q35 $([ -w /dev/kvm ] && echo '-accel kvm -cpu host' || echo '-accel tcg')}"
# Input fed to the shell over serial, and the marker its evaluation must print.
INPUT="${INPUT:-print[[SHELL_ALIVE_9271]]}"
MARKER="${MARKER:-SHELL_ALIVE_9271}"
IMG="${IMG:-boot.img}"
out="$(mktemp)"

# Optionally attach a data disk (ext2) so tests can exercise the disk/fs
# libraries and run()-from-disk. Enabled by setting DISK. DISK_IF picks the
# controller: nvme (default — q35 has no legacy IDE, matching the XPS) or ide
# (with QEMU_FLAGS="-machine pc ...", to keep ata.c covered).
DISK="${DISK:-}"
DISK_IF="${DISK_IF:-nvme}"
# NIC=1 attaches a user-mode e1000. Tests must opt in: QEMU's *default* NIC is
# an e1000 on the pc machine but an e1000e on q35, which the kernel's driver
# does not claim — relying on the default is a silent no-NIC boot.
NIC="${NIC:-}"
nic_args=()
if [ -n "$NIC" ]; then
    nic_args=(-nic user,model=e1000)
fi
disk_args=()
if [ -n "$DISK" ] && [ -f "$DISK" ]; then
    if [ "$DISK_IF" = ide ]; then
        disk_args=(-drive "file=$DISK,format=raw,if=none,id=juampidisk" \
            -device ide-hd,drive=juampidisk,bus=ide.0,unit=1)
    else
        disk_args=(-drive "file=$DISK,format=raw,if=none,id=juampidisk" \
            -device nvme,serial=juampidisk,drive=juampidisk)
    fi
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
} | timeout 60 "$QEMU" -bios "$ovmf_copy" $QEMU_FLAGS \
    -drive file="$IMG",format=raw,if=none,id=jboot \
    -device ide-hd,drive=jboot,bootindex=0 -m 512 \
    -smp "${QEMU_SMP:-4}" \
    "${disk_args[@]}" "${nic_args[@]}" \
    -vga none -display none -serial stdio -no-reboot >"$out" 2>&1

echo "--- serial output ---"
cat "$out"
echo "---------------------"
rc=1
# Anchored at line start: the marker must open an *evaluated* output line
# (optionally with trailing detail, e.g. "PARALLEL_OK  speedup 3.9x"). A bare
# substring match can false-pass on the input echo — e.g. when OVMF drops into
# its UEFI shell, which echoes whatever we typed; echo lines never start with
# the bare marker (the prompt and highlighting escapes precede it).
if grep -qE "^${MARKER}([[:space:]]|\$)" "$out"; then
    echo "PASS: shell booted and evaluated input ('$MARKER')"
    rc=0
else
    echo "FAIL: marker '$MARKER' not found (boot or shell failed)" >&2
fi
rm -f "$out" "$ovmf_copy"
exit $rc
