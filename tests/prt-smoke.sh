#!/usr/bin/env bash
set -uo pipefail

# Boot with an AC'97 device attached and verify that the completion interrupt is
# routed through the ACPI _PRT (not just the firmware Interrupt Line register),
# then play a tone and confirm completion IRQs actually fire. This exercises the
# uacpi _PRT path: acpi_pci_route() resolves the INTx pin -> GSI via the
# namespace (evaluating link-device _CRS), and the IOAPIC delivers it.

QEMU="${QEMU:-qemu-system-x86_64}"
# Machine realism: q35 (PCIe, like the XPS target) and, when KVM is available,
# the host CPU model (real CPUID/MSRs/PMU). CI has no /dev/kvm -> TCG fallback.
QEMU_FLAGS="${QEMU_FLAGS:--machine q35 $([ -w /dev/kvm ] && echo '-accel kvm -cpu host' || echo '-accel tcg')}"
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

# Play a tone, wait for a batch of completion interrupts, then report the count.
{
    sleep 10
    printf 'audio.tone(440,1500)\r'
    sleep 3
    printf 'print("IRQCHK", select(4, audio.info()))\r'
    sleep 3
} | timeout 60 "$QEMU" -bios "$ovmf_copy" $QEMU_FLAGS \
    -drive file="$IMG",format=raw,if=none,id=jboot \
    -device ide-hd,drive=jboot,bootindex=0 -m 512 \
    -smp "${QEMU_SMP:-4}" \
    -audiodev none,id=snd -device AC97,audiodev=snd \
    -vga none -display none -serial stdio -no-reboot >"$out" 2>&1

echo "--- serial output ---"
cat "$out"
echo "---------------------"
rc=0
if grep -q "ac97 intx gsi=.*(_PRT)" "$out"; then
    echo "PASS: AC'97 interrupt routed via ACPI _PRT"
else
    echo "FAIL: no '_PRT' route line (fell back to Interrupt Line, or no device)" >&2
    rc=1
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
