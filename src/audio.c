// Device-independent software mixer (see audio.h). Renders active voices into
// the backend's period buffers at a fixed 48 kHz / stereo / s16 format. A voice
// is a PCM source; for milestone 1 the only kind is a synthesized sine tone.
// The mixer is backend-agnostic — AC'97 today, HD Audio later — and is pumped
// from the idle loops so playback stays fed without blocking the shell.

#include <audio.h>
#include <utils.h>

extern const audio_output ac97_backend;

static const audio_output* backend;

// A 256-entry sine table, one period, amplitude +/-AMP. Built once at init with
// a Bhaskara approximation so the per-sample mixer path stays integer-only (no
// FP in audio_pump, which may run in interrupt context later).
#define SINE_LEN 256
#define SINE_AMP 10000
static int16_t sine_tab[SINE_LEN];

// A playing voice. `phase` is 16.16 fixed point in sine-table units (top bits
// index the table); `inc` advances it by freq/rate per frame. gain is Q8.
#define MAX_VOICES 8
typedef struct {
    bool active;
    uint32_t phase, inc;
    uint32_t frames_left;
    int32_t gain; // Q8 (256 = unity)
} voice;
static voice voices[MAX_VOICES];

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
    return ac97_backend.fail_reason();
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
    if (ac97_backend.init()) {
        backend = &ac97_backend;
        backend->start();
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

void audio_stop(int voice_id)
{
    if (voice_id < 0) {
        for (int i = 0; i < MAX_VOICES; i++) {
            voices[i].active = false;
        }
    } else if (voice_id < MAX_VOICES) {
        voices[voice_id].active = false;
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

// Render exactly `frames` stereo frames of mixed voice output into `out`.
static void mix_period(int16_t* out, uint32_t frames)
{
    for (uint32_t f = 0; f < frames; f++) {
        int32_t acc = 0;
        for (int v = 0; v < MAX_VOICES; v++) {
            if (!voices[v].active) {
                continue;
            }
            uint32_t idx = (voices[v].phase >> 16) & (SINE_LEN - 1);
            acc += (sine_tab[idx] * voices[v].gain) >> 8;
            voices[v].phase += voices[v].inc;
            if (--voices[v].frames_left == 0) {
                voices[v].active = false;
            }
        }
        int16_t s = clamp16(acc);
        out[2 * f] = s;     // left
        out[2 * f + 1] = s; // right (mono tone duplicated)
    }
}

void audio_pump(void)
{
    if (backend == NULL) {
        return;
    }
    int16_t* p;
    while ((p = backend->next_period()) != NULL) {
        mix_period(p, backend->period_frames());
        backend->commit_period();
    }
}
