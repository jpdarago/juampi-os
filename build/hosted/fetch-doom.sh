#!/usr/bin/env bash
# Fetch the doomgeneric engine sources into build/hosted/doom/ for `make doom`.
# The engine is GPL, so it is NOT committed (gitignored); only our frontend
# doomgeneric_juampi.c lives there in git. Idempotent: skips if already present.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DST="$ROOT/build/hosted/doom"

if [ -f "$DST/doomdef.c" ]; then
    echo "doomgeneric already fetched in $DST"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
git clone --depth 1 https://github.com/ozkl/doomgeneric "$tmp/dg"

mkdir -p "$DST"
# Copy the engine .c/.h, but keep only OUR platform frontend and no sound
# backend (we build Doom silent for now — see i_sound.c FEATURE_SOUND).
for f in "$tmp/dg/doomgeneric"/*.c "$tmp/dg/doomgeneric"/*.h; do
    b="$(basename "$f")"
    case "$b" in
    doomgeneric_juampi.c) continue ;;               # ours; never overwrite
    doomgeneric_*.c) continue ;;                     # other frontends (sdl/win/...)
    i_allegro*.c | i_sdl*.c) continue ;;             # audio backends
    esac
    cp "$f" "$DST/$b"
done
echo "doomgeneric engine fetched into $DST"
