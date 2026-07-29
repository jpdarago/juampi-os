#!/usr/bin/env bash
set -uo pipefail

# Boot with an Intel HD Audio controller + codec attached (no AC'97) and verify
# the HDA backend comes up: it resets the controller, sets up the CORB/RIRB
# rings, enumerates the codec (finds a DAC + output pin), routes its completion
# interrupt (ideally via the ACPI _PRT), and actually takes interrupts while a
# tone plays. This is the QEMU stand-in for the real XPS audio controller.

QEMU="${QEMU:-qemu-system-x86_64}"
IMG="${IMG:-boot.img}"
out="$(mktemp)"

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "error: $QEMU not found" >&2
    exit 127
fi

OVMF_FD="${OVMF_FD:-$(nix build --no-link --print-out-paths nixpkgs#OVMF.fd 2>/dev/null)/FV/OVMF.fd}"
if [ ! -f "$OVMF_FD" ]; then
    echo "error: OVMF firmware not found at '$OVMF_FD'" >&2
    exit 127
fi
ovmf_copy="$(mktemp)"
cp "$OVMF_FD" "$ovmf_copy"
chmod +w "$ovmf_copy"

# Play a tone, wait for a batch of completion interrupts, then report backend +
# count. Note: only an HDA controller is attached (no -device AC97), so the
# backend selection must land on "hda".
{
    sleep 10
    printf 'print("BACKEND", audio.backend and audio.backend() or (select(2, audio.info())))\r'
    sleep 1
    printf 'audio.tone(440,1500)\r'
    sleep 3
    printf 'print("IRQCHK", select(4, audio.info()))\r'
    sleep 3
} | timeout 60 "$QEMU" -bios "$ovmf_copy" \
    -drive file="$IMG",format=raw -m 512 \
    -smp "${QEMU_SMP:-4}" \
    -audiodev none,id=snd -device ich9-intel-hda -device hda-output,audiodev=snd \
    -vga none -display none -serial stdio -no-reboot >"$out" 2>&1

echo "--- serial output ---"
cat "$out"
echo "---------------------"
rc=0
if grep -q "hda codec" "$out"; then
    echo "PASS: HDA controller + codec enumerated"
else
    echo "FAIL: HDA codec never enumerated (no 'hda codec' line)" >&2
    rc=1
fi
if grep -qE "BACKEND[[:space:]]+hda" "$out"; then
    echo "PASS: mixer selected the HDA backend"
else
    echo "FAIL: HDA was not the selected backend" >&2
    rc=1
fi
if grep -q "hda intx gsi=.*(_PRT)" "$out"; then
    echo "PASS: HDA interrupt routed via ACPI _PRT"
else
    echo "WARN: HDA interrupt not routed via _PRT (using Interrupt Line?)" >&2
fi
irqs="$(grep -oE 'IRQCHK[[:space:]]+[0-9]+' "$out" | grep -oE '[0-9]+$' | tail -1)"
if [ -n "$irqs" ] && [ "$irqs" -gt 0 ]; then
    echo "PASS: completion interrupts fired (count=$irqs)"
else
    echo "FAIL: no completion interrupts (count='${irqs:-none}')" >&2
    rc=1
fi
rm -f "$out" "$ovmf_copy"
exit $rc
