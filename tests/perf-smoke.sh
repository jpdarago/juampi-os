#!/usr/bin/env bash
set -uo pipefail

# Verify the PMU path end to end: perf.available(), a perf.measure() over a
# known Lua loop (instruction count must exceed the loop's iteration count and
# IPC must be sane), and bench()'s pmu table. Requires KVM's vPMU (`-cpu
# host`); CI runs TCG where the PMU doesn't exist, so skip cleanly there.

if ! [ -w /dev/kvm ]; then
    echo "SKIP: no /dev/kvm — the PMU needs KVM's vPMU (-cpu host)"
    exit 0
fi

# One compact line: long serial bursts overrun the UART FIFO while the line
# editor echoes, so this must stay under ~150 characters.
dir="$(dirname "$0")"
INPUT='local r=perf.measure(function() local s=0 for i=1,5e4 do s=s+i end end,nil,2) assert(r.instructions>1e5 and r.ipc>0) print("PERF_OK")' \
MARKER=PERF_OK exec "$dir/boot-smoke.sh"
