// Device-independent software mixer (see audio.h). Renders active voices into
// the backend's period buffers at a fixed 48 kHz / stereo / s16 format. A voice
// is a PCM source; for milestone 1 the only kind is a synthesized sine tone.
// The mixer is backend-agnostic — AC'97 today, HD Audio later — and is pumped
// from the idle loops so playback stays fed without blocking the shell.

#include <audio.h>
#include <memory.h> // heap for owned PCM voice buffers
#include <utils.h>

extern const audio_output hda_backend;
extern const audio_output ac97_backend;

// Output backends tried in order at init; the first whose probe succeeds wins.
// HDA is the modern controller (and the real-hardware target); AC'97 is the
// QEMU-friendly fallback.
static const audio_output* const backends[] = {&hda_backend, &ac97_backend};

static const audio_output* backend;

// A 256-entry sine table, one period, amplitude +/-AMP. Built once at init with
// a Bhaskara approximation so the per-sample mixer path stays integer-only (no
// FP in audio_pump, which may run in interrupt context later).
#define SINE_LEN 256
#define SINE_AMP 10000
static int16_t sine_tab[SINE_LEN];

// A playing voice: either a synthesized sine tone or a PCM sample stream. gain
// is Q8 (256 = unity). Both advance a fixed-point cursor per output frame.
//
//  - TONE: `phase`/`inc` are 16.16 in sine-table units; `frames_left` counts
//    down the note.
//  - PCM: `pcm` is an owned interleaved-s16 buffer at `src_rate`/`channels`;
//    `pos`/`step` are a 16.16 fixed-point source-frame cursor advanced by
//    src_rate/AUDIO_RATE per output frame, linearly interpolated and (for mono)
//    duplicated to stereo. Freed when it finishes or is stopped.
#define MAX_VOICES 8
typedef enum { VOICE_TONE, VOICE_PCM } voice_kind;
typedef struct {
    bool active;
    voice_kind kind;
    int32_t gain; // Q8 (256 = unity)
    bool loop;
    // tone
    uint32_t phase, inc, frames_left;
    // pcm
    int16_t* pcm; // owned, heap
    uint32_t nframes;
    uint8_t channels;
    uint64_t pos;  // 16.16 fixed-point source-frame cursor
    uint32_t step; // source frames per output frame, 16.16
} voice;
static voice voices[MAX_VOICES];

// Release a voice's resources and mark it free. Deactivate FIRST so a
// completion ISR (which skips inactive voices) can never touch the buffer being
// freed. Only ever called from non-IRQ context (audio_play_pcm / audio_stop).
static void voice_free(voice* v)
{
    v->active = false;
    if (v->pcm != NULL) {
        heap_free(heap_default(), v->pcm);
        v->pcm = NULL;
    }
}

// Reclaim PCM buffers of voices that finished in the pump path (which, being
// IRQ-safe, marks them inactive but never frees). Non-IRQ context only.
static void reap_finished(void)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active && voices[i].pcm != NULL) {
            heap_free(heap_default(), voices[i].pcm);
            voices[i].pcm = NULL;
        }
    }
}

bool audio_present(void)
{
    return backend != NULL;
}
const char* audio_backend_name(void)
{
    return backend != NULL ? backend->name : "none";
}
const char* audio_fail_reason(void)
{
    if (backend != NULL) {
        return backend->fail_reason != NULL ? backend->fail_reason() : NULL;
    }
    // No backend came up: report the first probe that got far enough to leave a
    // reason (e.g. an HDA controller present but with no usable codec).
    for (unsigned i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        const char* why =
                backends[i]->fail_reason ? backends[i]->fail_reason() : NULL;
        if (why != NULL) {
            return why;
        }
    }
    return NULL;
}
bool audio_irq_driven(void)
{
    return backend != NULL && backend->irq_driven != NULL &&
           backend->irq_driven();
}
uint64_t audio_irq_count(void)
{
    return backend != NULL && backend->irq_count != NULL ? backend->irq_count()
                                                         : 0;
}

// Bhaskara I sine approximation on [0, PI]; good to ~0.2%, plenty for a table.
static double bhaskara(double x)
{
    const double PI = 3.14159265358979323846;
    double t = x * (PI - x);
    return (16.0 * t) / (5.0 * PI * PI - 4.0 * t);
}

static void build_sine(void)
{
    const double PI = 3.14159265358979323846;
    for (int i = 0; i < SINE_LEN; i++) {
        double x = (2.0 * PI * i) / SINE_LEN; // [0, 2PI)
        double s = x <= PI ? bhaskara(x) : -bhaskara(x - PI);
        sine_tab[i] = (int16_t)(s * SINE_AMP);
    }
}

void audio_init(void)
{
    build_sine();
    for (unsigned i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        if (!backends[i]->init()) {
            continue;
        }
        backend = backends[i];
        // Feed playback from the completion ISR (idle-loop pump is the
        // backstop).
        if (backend->enable_irq != NULL) {
            backend->enable_irq(audio_pump);
        }
        backend->start();
        return;
    }
}

int audio_tone(uint32_t freq, uint32_t ms, float gain)
{
    if (backend == NULL || freq == 0) {
        return -1;
    }
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].active) {
            continue;
        }
        voices[i].kind = VOICE_TONE;
        voices[i].loop = false;
        voices[i].pcm = NULL;
        voices[i].phase = 0;
        // per-frame phase step, 16.16 in table units: freq * SINE_LEN / rate
        voices[i].inc =
                (uint32_t)(((uint64_t)freq * SINE_LEN << 16) / AUDIO_RATE);
        voices[i].frames_left = (uint32_t)((uint64_t)ms * AUDIO_RATE / 1000);
        int32_t g = (int32_t)(gain * 256.0f);
        voices[i].gain = g < 0 ? 0 : (g > 256 ? 256 : g);
        voices[i].active = true;
        return i;
    }
    return -1; // no free voice
}

// Play `frames` of interleaved signed-16 PCM at `rate` Hz with `ch` channels
// (1 or 2). The samples are copied into a voice-owned buffer, resampled to the
// mixer rate and (for mono) fanned to stereo. Returns a voice id or -1.
int audio_play_pcm(const int16_t* samples, uint32_t frames, uint32_t rate,
                   uint8_t ch, bool loop, float gain)
{
    if (backend == NULL || samples == NULL || frames == 0 || rate == 0 ||
        (ch != 1 && ch != 2)) {
        return -1;
    }
    reap_finished(); // free buffers of voices that ended in the pump
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].active) {
            continue;
        }
        size_t bytes = (size_t)frames * ch * sizeof(int16_t);
        int16_t* buf = alloc(&heap_default()->base, 1, 1, (ptrdiff_t)bytes);
        if (buf == NULL) {
            return -1;
        }
        memcpy(buf, samples, bytes);
        voices[i].kind = VOICE_PCM;
        voices[i].pcm = buf;
        voices[i].nframes = frames;
        voices[i].channels = ch;
        voices[i].loop = loop;
        voices[i].pos = 0;
        voices[i].step = (uint32_t)(((uint64_t)rate << 16) / AUDIO_RATE);
        int32_t g = (int32_t)(gain * 256.0f);
        voices[i].gain = g < 0 ? 0 : (g > 256 ? 256 : g);
        voices[i].active = true;
        return i;
    }
    return -1;
}

void audio_stop(int voice_id)
{
    if (voice_id < 0) {
        for (int i = 0; i < MAX_VOICES; i++) {
            voice_free(&voices[i]);
        }
    } else if (voice_id < MAX_VOICES) {
        voice_free(&voices[voice_id]);
    }
}

static inline int16_t clamp16(int32_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (int16_t)v;
}

// Linearly interpolate channel `c` of a PCM voice at its fractional cursor.
static int32_t pcm_sample(const voice* v, uint32_t idx, uint8_t c,
                          uint32_t frac)
{
    const int16_t* p = v->pcm;
    uint32_t j =
            idx + 1 < v->nframes ? idx + 1 : idx; // clamp at the last frame
    int32_t a = p[idx * v->channels + c];
    int32_t b = p[j * v->channels + c];
    return a + (((b - a) * (int32_t)frac) >> 16);
}

// Accumulate one voice's contribution to a stereo output frame into l/r, and
// advance its cursor. Deactivates (freeing PCM) when the voice ends.
static void voice_mix(voice* v, int32_t* l, int32_t* r)
{
    if (v->kind == VOICE_TONE) {
        uint32_t idx = (v->phase >> 16) & (SINE_LEN - 1);
        int32_t s = (sine_tab[idx] * v->gain) >> 8;
        *l += s;
        *r += s;
        v->phase += v->inc;
        if (--v->frames_left == 0) {
            v->active = false;
        }
        return;
    }
    // PCM
    uint32_t idx = (uint32_t)(v->pos >> 16);
    if (idx >= v->nframes) {
        if (v->loop) {
            v->pos = 0;
            idx = 0;
        } else {
            // Runs in the ISR: mark done but don't free here (no heap in the
            // pump path); reap_finished() reclaims the buffer in a safe
            // context.
            v->active = false;
            return;
        }
    }
    uint32_t frac = (uint32_t)(v->pos & 0xFFFF);
    int32_t sl = pcm_sample(v, idx, 0, frac);
    int32_t sr = v->channels == 2 ? pcm_sample(v, idx, 1, frac) : sl;
    *l += (sl * v->gain) >> 8;
    *r += (sr * v->gain) >> 8;
    v->pos += v->step;
}

// Render exactly `frames` stereo frames of mixed voice output into `out`.
static void mix_period(int16_t* out, uint32_t frames)
{
    for (uint32_t f = 0; f < frames; f++) {
        int32_t l = 0, r = 0;
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].active) {
                voice_mix(&voices[v], &l, &r);
            }
        }
        out[2 * f] = clamp16(l);
        out[2 * f + 1] = clamp16(r);
    }
}

void audio_pump(void)
{
    // Reentrancy guard: the completion ISR and the idle loop both call this.
    // On this single-core path a plain flag suffices — if the ISR fires while
    // the idle pump holds it, the ISR simply skips (the ring has ~64 ms slack).
    static volatile bool pumping;
    if (backend == NULL || pumping) {
        return;
    }
    pumping = true;
    int16_t* p;
    while ((p = backend->next_period()) != NULL) {
        mix_period(p, backend->period_frames());
        backend->commit_period();
    }
    pumping = false;
}
