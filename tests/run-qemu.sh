#!/usr/bin/env bash
set -uo pipefail

# Run a KTEST kernel under QEMU headless and translate the isa-debug-exit
# status into a normal 0/1 process exit code for make/CI.
#
# The kernel prints test output to the debug console (port 0xE9), which
# `-debugcon stdio` streams to our stdout, and signals the result via the
# isa-debug-exit device (port 0xf4): QEMU exits with ((value << 1) | 1), so
# KTEST_PASS (0x10) -> 33 and KTEST_FAIL (0x11) -> 35.

KERNEL="${1:-kernel-test.bin}"
QEMU="${QEMU:-qemu-system-i386}"
TIMEOUT="${TIMEOUT:-30}"
PASS_CODE=33

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "error: $QEMU not found (install qemu-system-x86 / qemu)" >&2
    exit 127
fi

QEMU_ARGS=(
    -kernel "$KERNEL"
    -m 128
    -display none
    -no-reboot
    -debugcon stdio
    -serial null
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
)

# `timeout` is GNU coreutils; on macOS it's `gtimeout` (brew install coreutils).
# Fall back to running without a watchdog if neither is present.
if command -v timeout >/dev/null 2>&1; then
    timeout --foreground "$TIMEOUT" "$QEMU" "${QEMU_ARGS[@]}"
elif command -v gtimeout >/dev/null 2>&1; then
    gtimeout --foreground "$TIMEOUT" "$QEMU" "${QEMU_ARGS[@]}"
else
    echo "warning: no timeout/gtimeout found; running without a watchdog" >&2
    "$QEMU" "${QEMU_ARGS[@]}"
fi
status=$?

if [ "$status" -eq "$PASS_CODE" ]; then
    echo "PASS (qemu exit $status)"
    exit 0
elif [ "$status" -eq 124 ]; then
    echo "FAIL: timed out after ${TIMEOUT}s" >&2
    exit 1
else
    echo "FAIL: tests failed or kernel crashed (qemu exit $status)" >&2
    exit 1
fi
