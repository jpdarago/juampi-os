#!/usr/bin/env bash
set -uo pipefail

# Prove the UDP socket path receives datagrams. QEMU user-mode (SLIRP) forwards a
# host UDP port to the guest (hostfwd); we bind that guest port, send a datagram
# from the host with bash's /dev/udp, and assert the guest's recvfrom() returns
# the payload. Fully self-contained — no external network or DNS needed, so it is
# CI-safe. Exercises Ethernet -> IPv4 -> UDP -> socket demux -> Lua.

QEMU="${QEMU:-qemu-system-x86_64}"
IMG="${IMG:-boot.img}"
MARKER="HELLOUDP42"
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

GUEST='s=net.udp() print("BOUND",s:bind(9999)) local d,ip,p=s:recvfrom(20000) print("UDPRX",d,ip,p) s:close()'

# Bind the guest port, then (concurrently) fire host datagrams at the forwarded
# port. Hold the pipe open until QEMU is killed by timeout (see boot-smoke.sh).
{
    sleep 10
    printf '%s\r' "$GUEST"
    ( sleep 5
      for _ in 1 2 3 4 5; do
          printf '%s' "$MARKER" >/dev/udp/127.0.0.1/15353 2>/dev/null
          sleep 1
      done ) &
    sleep 22
} | timeout 60 "$QEMU" -bios "$ovmf_copy" \
    -drive file="$IMG",format=raw -m 512 \
    -smp "${QEMU_SMP:-4}" \
    -netdev user,id=n0,hostfwd=udp:127.0.0.1:15353-10.0.2.15:9999 \
    -device e1000,netdev=n0 \
    -vga none -display none -serial stdio -no-reboot >"$out" 2>&1

echo "--- serial output (udp lines) ---"
tr -d '\r' <"$out" | grep -aE 'net e1000|BOUND|UDPRX'
echo "---------------------------------"
rc=1
if tr -d '\r' <"$out" | grep -q "UDPRX	$MARKER"; then
    echo "PASS: UDP socket bound and received the datagram ('$MARKER')"
    rc=0
else
    echo "FAIL: guest did not receive the UDP datagram" >&2
fi
rm -f "$out" "$ovmf_copy"
exit $rc
