#!/usr/bin/env bash
set -uo pipefail

# Prove the TCP stack end to end: the guest opens a listening socket, and a host
# client (socat) connects into it via QEMU user-mode (SLIRP) hostfwd, sends a
# line, and reads the guest's reply. This exercises the full machinery — 3-way
# handshake (receive SYN, send SYN-ACK, receive ACK), in-order data both ways,
# checksums, and close. Self-contained (host loopback + hostfwd), so CI-safe.

QEMU="${QEMU:-qemu-system-x86_64}"
IMG="${IMG:-boot.img}"
out="$(mktemp)"
hostlog="$(mktemp)"

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "error: $QEMU not found" >&2
    exit 127
fi
if ! command -v socat >/dev/null 2>&1; then
    echo "error: socat not found" >&2
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

# Guest: listen on 9999, accept, receive a line, reply "PONG:<line>", close.
G='l=net.listen(9999) print("LISTEN",l~=nil) l:accept(20000) local d=l:recv(300,5000) print("TCPGOT",d) l:send("PONG:"..(d or "")) l:close()'

{
    sleep 10
    printf '%s\r' "$G"
    # Concurrently: host client connects into the guest via the forwarded port.
    ( sleep 5
      R=$(printf 'PING123' | socat -t6 - TCP:127.0.0.1:15080 2>/dev/null)
      echo "HOSTRESP=[$R]" >"$hostlog" ) &
    sleep 20
} | timeout 70 "$QEMU" -bios "$ovmf_copy" \
    -drive file="$IMG",format=raw -m 512 \
    -smp "${QEMU_SMP:-4}" \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:15080-10.0.2.15:9999 \
    -device e1000,netdev=n0 \
    -vga none -display none -serial stdio -no-reboot >"$out" 2>&1

echo "--- guest ---"; tr -d '\r' <"$out" | grep -aE 'net e1000|LISTEN|TCPGOT'
echo "--- host  ---"; cat "$hostlog"
rc=1
if grep -q 'HOSTRESP=\[PONG:PING123\]' "$hostlog"; then
    echo "PASS: TCP handshake, bidirectional data, and close all worked"
    rc=0
else
    echo "FAIL: TCP round-trip did not complete" >&2
fi
rm -f "$out" "$hostlog" "$ovmf_copy"
exit $rc
