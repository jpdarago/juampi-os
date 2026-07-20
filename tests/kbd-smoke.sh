#!/usr/bin/env bash
set -uo pipefail

# Boot the image and drive the shell through the PS/2 keyboard: inject
# keystrokes with QMP send-key typing an `echo` command, then assert the marker
# appears twice in the serial log (the console mirrors the screen): once as the
# echoed keystrokes, once as the evaluated output. This proves the whole
# keyboard -> IRQ -> ring buffer -> console -> shell path.

QEMU="${QEMU:-qemu-system-x86_64}"
IMG="${IMG:-boot.img}"
MARKER="kbdalive4417"
out="$(mktemp)"
sock="$(mktemp -u)"

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "error: $QEMU not found" >&2
    exit 127
fi
if ! command -v socat >/dev/null 2>&1; then
    echo "error: socat not found (needed to talk QMP)" >&2
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

timeout 60 "$QEMU" -bios "$ovmf_copy" \
    -drive file="$IMG",format=raw -m 512 \
    -vga none -display none -serial file:"$out" \
    -qmp unix:"$sock",server,nowait -no-reboot &
qpid=$!
# Let the kernel reach the shell before typing. Keystrokes are IRQ-buffered
# (keyboard.c ring), so a little early is harmless, but give boot margin.
sleep 12

send_keys() {
    {
        printf '{"execute":"qmp_capabilities"}\n'
        sleep 0.2
        for k in "$@"; do
            printf '{"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":"%s"}]}}\n' "$k"
            sleep 0.15
        done
        sleep 0.3
    } | socat - UNIX-CONNECT:"$sock" >/dev/null 2>&1
}

# Type a Lua expression using only unshifted keys (long-bracket string, no
# parens/quotes): print[[kbdalive4417]]<enter>
send_keys p r i n t bracket_left bracket_left \
    k b d a l i v e 4 4 1 7 \
    bracket_right bracket_right ret
sleep 2

kill "$qpid" 2>/dev/null
wait "$qpid" 2>/dev/null

echo "--- serial output (tail) ---"
tail -5 "$out"
echo "----------------------------"
count=$(grep -c "$MARKER" "$out" || true)
rm -f "$out" "$ovmf_copy"
if [ "$count" -ge 2 ]; then
    echo "PASS: keyboard input typed, echoed and evaluated ('$MARKER' x$count)"
    exit 0
fi
echo "FAIL: expected '$MARKER' twice (echo + eval), found $count" >&2
exit 1
