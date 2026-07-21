#!/usr/bin/env bash
set -uo pipefail

# Boot the image with an e1000 NIC on QEMU user-mode (SLIRP) networking and
# prove the vertical slice of the network stack end to end: bring-up, our
# address, and an ICMP ping of the SLIRP gateway (10.0.2.2, answered internally
# by SLIRP — no host privileges needed). This exercises the whole path: e1000
# DMA rings -> Ethernet -> ARP -> IPv4 -> ICMP -> back up to Lua.

QEMU="${QEMU:-qemu-system-x86_64}"
IMG="${IMG:-boot.img}"
MARKER="NET_PING_OK"
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

# Assert bring-up (net.ip) and a successful ping, printing stable markers. Hold
# the input pipe open until QEMU is stopped by timeout (see boot-smoke.sh).
{
    sleep 10
    printf 'print("NET_IP", net.ip())\r'
    sleep 1
    printf 'print(net.ping("10.0.2.2") and "NET_PING_OK" or "NET_PING_FAIL")\r'
    sleep 4
} | timeout 60 "$QEMU" -bios "$ovmf_copy" \
    -drive file="$IMG",format=raw -m 512 \
    -smp "${QEMU_SMP:-4}" \
    -nic user,model=e1000 \
    -vga none -display none -serial stdio -no-reboot >"$out" 2>&1

echo "--- serial output (net lines) ---"
grep -iE 'net e1000|no NIC|NET_IP|NET_PING' "$out"
echo "---------------------------------"
rc=1
if grep -q "$MARKER" "$out"; then
    echo "PASS: NIC up and ping of the SLIRP gateway succeeded ('$MARKER')"
    rc=0
else
    echo "FAIL: '$MARKER' not found (NIC bring-up or ping failed)" >&2
fi
rm -f "$out" "$ovmf_copy"
exit $rc
