# Vendored doomgeneric (GPLv2)

This directory vendors the [doomgeneric](https://github.com/ozkl/doomgeneric)
Doom engine (id Software's Doom source + Simon Howard's Chocolate Doom cleanups +
ozkl's generic frontend). **These files are licensed under the GNU General Public
License v2** (see the copyright headers in each source file), *not* the MIT
license the rest of juampiOS uses. Distributing juampiOS therefore means offering
this component under the GPL — which is satisfied by shipping this source.

- `doomgeneric_juampi.c` is **our** platform frontend (the juampiOS glue), the
  one file here that is part of juampiOS proper.
- The other frontends (SDL/X11/Windows/…) and the SDL/Allegro sound backends
  from upstream are removed; Doom is built silent (no `FEATURE_SOUND`).
- `../fetch-doom.sh` re-fetches/updates this vendored copy from upstream.

The game data (`doom1.wad`) is **not** here — it's fetched onto the ext2 disk by
`make doom-wad`. See `docs/hosted-libc.md`.
