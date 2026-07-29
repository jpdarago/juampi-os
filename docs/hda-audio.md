# Intel HD Audio backend (`src/hda.c`)

The audio mixer (`src/audio.c`) drives a pluggable `audio_output` backend (a
cyclic ring of period buffers the hardware DMAs; see `include/audio.h`). AC'97
(`src/ac97.c`) was the first backend, but it's a QEMU/legacy device — no modern
machine ships it. The target **Dell XPS 15 9510** (and essentially every current
x86 box) has an **Intel HD Audio (HDA)** controller instead, so `src/hda.c` adds
an HDA backend behind the same vtable. `audio_init()` probes HDA first, then
AC'97; the mixer, QOA codec and Lua bindings are unchanged.

## HDA vs AC'97

| | AC'97 | HDA |
|---|---|---|
| Register access | I/O ports (BAR0/BAR1) | MMIO (BAR0, mapped `PAGEF_UC`) |
| Codec control | fixed-function mixer regs | **verbs** over the CORB/RIRB rings |
| Playback DMA | 32-entry BDL, I/O-port cursor | stream descriptor + BDL, `SDLPIB` cursor |
| Interrupt | legacy PCI INTx | INTx (QEMU) / MSI (real HW, later) |

## Bring-up sequence (`hda_init`)

1. Find the controller by PCI class `0x04`/subclass `0x03`; enable bus-master +
   memory decode; `iomap` BAR0 (`pci_bar64`) uncached.
2. Reset the controller: `GCTL.CRST` 0 → 1, wait for it to read back 1, then
   wait for a codec to latch in `STATESTS` (~pick the first present address).
3. Compute the first **output** stream descriptor: descriptors are laid out
   `[inputs][outputs][bidir]`, so it starts at `0x80 + ISS*0x20` (ISS from GCAP).
   Its INTCTL/INTSTS bit is `1 << ISS`.
4. `ring_setup()` — CORB (command) + RIRB (response) rings, one page each, 256
   entries, DMA engines running.
5. `enumerate_codec()` — walk the node tree (root → function groups → widgets),
   find the first audio-output converter (DAC) and first output-capable pin.
6. `stream_setup()` — reset the stream, program stream tag/format (48 kHz s16
   stereo)/cyclic-buffer-length/LVI, point it at a BDL of fixed period buffers
   (silence), exactly like the AC'97 ring.
7. `configure_path()` — power up DAC+pin (D0), route the stream tag to the DAC,
   unmute both amps at full gain, enable the pin output driver + EAPD.

## CORB/RIRB gotcha (the one that bit)

Each verb is pushed to the CORB, then its response is polled from the RIRB. The
controller **stalls CORB processing once the unacknowledged response count
reaches `RINTCNT`** (`corb_run` bails on `rirb_count == rintcnt`). With the
initial `RINTCNT=1`, exactly one command worked and every later one timed out.
Fix: set a large `RINTCNT` **and** clear `RIRBSTS.RINTFL` after consuming each
response (which resets the count) — we poll rather than take RIRB interrupts.

## Interrupts

The completion IRQ reuses the AC'97 path: prefer the ACPI **`_PRT`**
(`acpi_pci_route` → correct GSI + level/active-low polarity via `irq_route_gsi`),
falling back to the PCI Interrupt Line register. Per-buffer `IOC` in the BDL +
`SDCTL.IOCE` + `INTCTL` (GIE|CIE|stream-bit); the ISR acks `SDSTS.BCIS` (W1C)
and refills. Idle-loop pump stays as the backstop.

## Verification (QEMU)

`tests/hda-smoke.sh` boots with `-device ich9-intel-hda -device hda-output` and
asserts: codec enumerated (`hda codec 0 dac 0x2 pin 0x3`), the mixer selected the
`hda` backend, the IRQ routed via `_PRT` (`hda intx gsi=11 (_PRT)`), and
completion interrupts fired while a tone played. A WAV capture
(`-audiodev wav`) of a 440 Hz tone measured **439.5 Hz** (ffmpeg zero-crossing
rate), confirming correct end-to-end output.

## Real hardware (XPS) — not yet attempted

The QEMU backend is the reusable foundation. On the 9510 two things differ and
are out of scope here:

- **Speakers** are driven by a separate smart-amplifier chip over I2S/TDM from
  the audio DSP — they need Intel SOF firmware + I2C amp init, far beyond a
  bare-metal driver. The **headphone jack** (a DAC in the Realtek codec) is the
  realistic target, reachable with the legacy HDA verbs here *if* the codec
  enumerates on the HDA link (vs SoundWire / DSP-only mode).
- Real HDA prefers **MSI** over INTx; `pci_msix_setup` already exists for the
  NVMe/xHCI path and would slot in.
