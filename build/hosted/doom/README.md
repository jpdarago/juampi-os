# Vendored doomgeneric (GPLv2) + Nuked-OPL3 (LGPL 2.1)

This directory vendors the [doomgeneric](https://github.com/ozkl/doomgeneric)
Doom engine (id Software's Doom source + Simon Howard's Chocolate Doom cleanups +
ozkl's generic frontend). **These files are licensed under the GNU General Public
License v2** (see the copyright headers in each source file), *not* the MIT
license the rest of juampiOS uses. Distributing juampiOS therefore means offering
this component under the GPL — which is satisfied by shipping this source.

`opl3.c` / `opl3.h` are [Nuked-OPL3](https://github.com/nukeykt/Nuked-OPL3), a
cycle-accurate OPL3 FM emulator, **licensed under the LGPL 2.1** — used to
synthesize Doom's music (see `i_juampimusic.c`).

- `doomgeneric_juampi.c` (video/input/timing frontend), `i_juampisound.c` (SFX
  backend) and `i_juampimusic.c` (OPL music backend) are **our** juampiOS glue,
  the files here that are part of juampiOS proper.
- The other frontends (SDL/X11/Windows/…) and the SDL/Allegro sound backends from
  upstream are removed; sound/music come from our backends above, built with
  `-DFEATURE_SOUND`.
- `../fetch-doom.sh` re-fetches/updates the vendored doomgeneric copy from
  upstream; `opl3.c`/`opl3.h` are fetched from the Nuked-OPL3 repo.

The game data (`doom1.wad`) is **not** here — it's fetched onto the ext2 disk by
`make doom-wad`. See `docs/hosted-libc.md`.
