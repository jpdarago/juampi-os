#ifndef __AUDIO_H
#define __AUDIO_H

#include <stdint.h>
#include <stdbool.h>

// Kernel audio: a device-independent software mixer over a pluggable output
// backend. The mixer runs at a fixed internal format (48 kHz, stereo, signed
// 16-bit interleaved) and renders active "voices" (tones now; decoded PCM /
// QOA later) into the backend's DMA period buffers. AC'97 is the backend today;
// Intel HD Audio can implement the same vtable later without touching the mixer
// or the Lua bindings — both are buffer-descriptor-list (period ring) devices.

#define AUDIO_RATE 48000 // mixer + backend sample rate, in Hz
#define AUDIO_CH 2       // channels (stereo)

// A PCM output backend: a cyclic list of fixed-size period buffers the hardware
// DMAs in a loop. The mixer fills free periods and commits them; the backend
// keeps the play cursor moving. All PCM is signed-16 interleaved at AUDIO_RATE.
typedef struct {
    const char* name;
    bool (*init)(void);               // probe + bring up; false if absent
    uint32_t (*period_frames)(void);  // stereo frames per period buffer
    int16_t* (*next_period)(void);    // a free period to fill, or NULL if full
    void (*commit_period)(void);      // queue the period last handed out
    void (*start)(void);              // begin DMA (idempotent)
    void (*stop)(void);               // halt DMA
    const char* (*fail_reason)(void); // why init failed, or NULL
} audio_output;

// Bring up audio: select a backend (AC'97), start its DMA. Safe to call with no
// device present (audio_present() stays false).
void audio_init(void);
bool audio_present(void);
const char* audio_backend_name(void); // "ac97" / "none"
const char* audio_fail_reason(void);  // backend init failure, or NULL

// Mix active voices into any free backend periods. Non-blocking; call from the
// idle loops (like net_poll/xhci_poll) so playback stays fed between events.
void audio_pump(void);

// Play a synthesized sine tone of `freq` Hz for `ms` milliseconds at `gain`
// (0..1). Returns a voice id (>=0) or -1 if no free voice / no device.
int audio_tone(uint32_t freq, uint32_t ms, float gain);

// Stop a voice (id from audio_tone), or all voices when id < 0.
void audio_stop(int voice);

#endif
