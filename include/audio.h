#ifndef __AUDIO_H
#define __AUDIO_H

#include <stdint.h>
#include <stdbool.h>

// Kernel audio: a device-independent software mixer over a pluggable output
// backend. The mixer runs at a fixed internal format (48 kHz, stereo, signed
// 16-bit interleaved) and renders active "voices" (tones, decoded PCM / QOA)
// into the backend's DMA period buffers. Two backends implement the vtable and
// are probed in order: Intel HD Audio (src/hda.c — the modern controller, and
// the real-hardware target) then AC'97 (src/ac97.c). Both are
// buffer-descriptor-list (period ring) devices, so neither the mixer nor the
// Lua bindings care which one is running.

#define AUDIO_RATE 48000 // mixer + backend sample rate, in Hz
#define AUDIO_CH 2       // channels (stereo)

// A PCM output backend: a cyclic list of fixed-size period buffers the hardware
// DMAs in a loop. The mixer fills free periods and commits them; the backend
// keeps the play cursor moving. All PCM is signed-16 interleaved at AUDIO_RATE.
struct audio_output {
    const char* name;
    bool (*init)(void);               // probe + bring up; false if absent
    uint32_t (*period_frames)(void);  // stereo frames per period buffer
    int16_t* (*next_period)(void);    // a free period to fill, or NULL if full
    void (*commit_period)(void);      // queue the period last handed out
    void (*start)(void);              // begin DMA (idempotent)
    void (*stop)(void);               // halt DMA
    const char* (*fail_reason)(void); // why init failed, or NULL
    // Route the device's completion interrupt to `refill` (the mixer's pump),
    // so playback is fed from the ISR rather than only the idle loops. NULL, or
    // a no-op when the device has no usable interrupt (then polling carries
    // it).
    void (*enable_irq)(void (*refill)(void));
    bool (*irq_driven)(void);    // true once interrupts are delivering
    uint64_t (*irq_count)(void); // completion interrupts taken
};

// Bring up audio: probe backends in order (HDA, then AC'97), start the DMA of
// the first present. Safe to call with no device (audio_present() stays false).
void audio_init(void);
bool audio_present(void);
const char* audio_backend_name(void); // "hda" / "ac97" / "none"
const char* audio_fail_reason(void);  // backend init failure, or NULL
bool audio_irq_driven(void);          // ISR-fed (false: polled from idle loops)
uint64_t audio_irq_count(void);       // completion interrupts taken

// Mix active voices into any free backend periods. Non-blocking and reentrancy-
// guarded; called from both the completion ISR and the idle loops (like
// net_poll/xhci_poll) so playback stays fed. Does no heap work (IRQ-safe).
void audio_pump(void);

// Play a synthesized sine tone of `freq` Hz for `ms` milliseconds at `gain`
// (0..1). Returns a voice id (>=0) or -1 if no free voice / no device.
int audio_tone(uint32_t freq, uint32_t ms, float gain);

// Play `frames` of interleaved signed-16 PCM at `rate` Hz with `ch` channels
// (1 or 2), resampled to the mixer format and (for mono) fanned to stereo. The
// samples are copied into a voice-owned buffer. `loop` repeats until stopped.
// Returns a voice id (>=0) or -1 if no free voice / no device / bad args.
int audio_play_pcm(const int16_t* samples, uint32_t frames, uint32_t rate,
                   uint8_t ch, bool loop, float gain);

// Stop a voice (id from audio_tone/audio_play_pcm), or all voices when id < 0.
void audio_stop(int voice);

// Whether a voice handle refers to a still-playing sound. False once it
// finished or its slot was recycled by a later sound (the handle carries a
// generation).
bool audio_voice_active(int voice);

// Streaming music: a single open-ended source (e.g. a hosted program's software
// synth) mixed in alongside the voices, fed through a lock-free
// single-producer/ single-consumer ring. Unlike a voice it never "ends" — the
// producer keeps it topped up. Samples must already be at the mixer format:
// AUDIO_RATE, stereo, s16 interleaved (the producer does any resampling).
//   start(): clear the ring and enable mixing.
//   write(): enqueue up to `frames` stereo frames; returns how many were
//            accepted (fewer if the ring is full) so the producer can retry.
//   space(): free stereo frames right now, so the producer can pace itself.
//   stop():  disable and drop any buffered audio.
void audio_music_start(void);
uint32_t audio_music_write(const int16_t* stereo, uint32_t frames);
uint32_t audio_music_space(void);
void audio_music_stop(void);

#endif
