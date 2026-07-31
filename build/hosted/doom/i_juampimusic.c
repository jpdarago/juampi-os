// juampiOS OPL music backend for doomgeneric (DG_music_module). Doom's music
// lumps are MUS (Id's compact MIDI variant); the original game played them on a
// Sound Blaster's OPL2/OPL3 FM chip, with the WAD's GENMIDI lump mapping each
// General-MIDI instrument to an FM patch. We reproduce that: parse the MUS event
// stream directly, drive a vendored cycle-accurate OPL3 emulator (Nuked-OPL3,
// opl3.c) via GENMIDI patches, render its 48 kHz stereo output, and push it to
// the kernel's streaming-music mixer (juampi_music_*). No external assets and no
// SDL/timidity: everything needed is already in doom1.wad.
//
// Design notes:
//  - We emulate an OPL running in OPL3 mode (so both stereo channels sound) but
//    use only its first 9 melodic voices — plenty for Doom, and it avoids the
//    second register bank. Notes beyond 9 steal the oldest voice.
//  - The song clock runs at the MUS tick rate (140 Hz). Poll() (called ~35 Hz
//    by the game) tops up the kernel ring: it generates OPL samples up to the
//    next event, processes that event batch, and repeats, converting ticks to
//    48 kHz samples with a fractional carry so timing doesn't drift.
//  - Pitch uses a standard equal-tempered OPL F-number table; block = octave-1.
//    base_note_offset (GENMIDI, 1/32-semitone units) and the MUS pitch wheel
//    (+/-2 semitones) refine it via linear interpolation between semitones.

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#include "opl3.h"

#include <juampi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OPL_RATE 48000    // matches the kernel mixer / streaming format
#define MUS_RATE 140      // MUS tick rate (Hz)
#define NUM_VOICES 9      // OPL melodic channels we use (bank 0)
#define MUS_PERCUSSION 15 // MUS channel 15 is percussion
#define GENMIDI_PERCUSSION_BASE 128 // GENMIDI entries 128..174 = notes 35..81

#define GENMIDI_FLAG_FIXED 0x0001  // fixed pitch (percussion): use fixed_note
#define GENMIDI_FLAG_2VOICE 0x0004 // two-voice instrument (we play voice 0 only)

// --- GENMIDI on-disk layout (packed) ----------------------------------------
struct genmidi_op {
    uint8_t tremolo;  // -> reg 0x20 (AM|VIB|EGT|KSR|MULT)
    uint8_t attack;   // -> reg 0x60 (attack|decay)
    uint8_t sustain;  // -> reg 0x80 (sustain|release)
    uint8_t waveform; // -> reg 0xE0
    uint8_t scale;    // key scale level, upper bits of reg 0x40
    uint8_t level;    // output level (TL), lower 6 bits of reg 0x40
} __attribute__((packed));

struct genmidi_voice {
    struct genmidi_op mod; // modulator operator
    uint8_t feedback;      // -> reg 0xC0 (feedback|connection)
    struct genmidi_op car; // carrier operator
    uint8_t unused;
    int16_t base_note_offset; // pitch offset, in 1/32-semitone units
} __attribute__((packed));

struct genmidi_instr {
    uint16_t flags;
    uint8_t fine_tuning;
    uint8_t fixed_note;
    struct genmidi_voice voices[2];
} __attribute__((packed));

static const struct genmidi_instr* genmidi; // 175 instruments, cached from WAD

// F-numbers for the 12 semitones of one octave; block (octave) is applied on
// top. Chosen so MIDI note 69 (A4) -> 440 Hz: 580 * 49716 / 2^(20-4) = 440.
static const uint16_t note_fnum[12] = {345, 365, 387, 410, 435, 460,
                                       488, 517, 547, 580, 615, 651};

// OPL operator base offsets for voices 0..8 (modulator; carrier = +3).
static const uint8_t op_off[NUM_VOICES] = {0x00, 0x01, 0x02, 0x08, 0x09,
                                           0x0A, 0x10, 0x11, 0x12};

// Perceptual volume curve (chocolate-doom's volume_mapping_table): maps a linear
// 0-127 MIDI volume to a 0-127 value that, used linearly against the OPL's
// logarithmic total-level register, gives even loudness. Without it, mid volumes
// land ~20 dB too quiet.
static const uint8_t volmap[128] = {
        0,   1,   3,   5,   6,   8,   10,  11,  13,  14,  16,  17,  19,  20,  22,
        23,  25,  26,  27,  29,  30,  32,  33,  34,  36,  37,  39,  41,  43,  45,
        47,  49,  50,  52,  54,  55,  57,  59,  60,  61,  63,  64,  66,  67,  68,
        69,  71,  72,  73,  74,  75,  76,  77,  79,  80,  81,  82,  83,  84,  84,
        85,  86,  87,  88,  89,  90,  91,  92,  92,  93,  94,  95,  96,  96,  97,
        98,  99,  99,  100, 101, 101, 102, 103, 103, 104, 105, 105, 106, 107, 107,
        108, 109, 109, 110, 110, 111, 112, 112, 113, 113, 114, 114, 115, 115, 116,
        117, 117, 118, 118, 119, 119, 120, 120, 121, 121, 122, 122, 123, 123, 123,
        124, 124, 125, 125, 126, 126, 127, 127};

// One OPL voice and what it is currently sounding.
struct opl_voice {
    boolean used;
    int mus_channel; // owning MUS channel
    int note;        // MUS note (for note-off matching)
    int play_note;   // note actually sounded (fixed-pitch instruments differ)
    const struct genmidi_voice* gv; // patch voice in use (for pitch/volume)
    boolean additive;               // connection bit: scale both operators
    uint32_t age;                   // for oldest-voice stealing
};
static struct opl_voice voices[NUM_VOICES];
static uint32_t voice_age;

// Per-MUS-channel state.
struct mus_channel {
    int instrument; // GM program (controller 0)
    int volume;     // channel volume (controller 3), 0-127
    int velocity;   // last note velocity
    int pitchbend;  // 0-255, 128 = centre
};
static struct mus_channel chan[16];

static opl3_chip chip;

// Registered song: a private copy of the MUS lump + its score offset.
struct song {
    uint8_t* data;
    int len;
    int score_start;
};

static const uint8_t* mus_data;
static int mus_len;
static int mus_pos;
static int mus_score_start;
static boolean playing;
static boolean paused;
static boolean looping;
static int music_volume = 127;
static long samples_until_event; // 48 kHz frames before the next event batch
static long tick_frac;           // remainder carry for tick->sample conversion

static int16_t genbuf[2048 * 2]; // scratch: up to 2048 stereo frames

static void write_reg(uint16_t reg, uint8_t val)
{
    OPL3_WriteRegBuffered(&chip, reg, val);
}

static void load_genmidi(void)
{
    if (genmidi != NULL) {
        return;
    }
    int lump = W_GetNumForName("GENMIDI");
    if (lump < 0) {
        return;
    }
    const uint8_t* d = W_CacheLumpNum(lump, PU_STATIC);
    // 8-byte "#OPL_II#" header precedes the 175 instrument records.
    genmidi = (const struct genmidi_instr*)(d + 8);
}

// Convert an 8.8 fixed-point note value to OPL F-number low/high (with block).
static void calc_fnum(int note_fixed, uint8_t* lo, uint8_t* hi)
{
    if (note_fixed < 0) {
        note_fixed = 0;
    }
    int semi = note_fixed >> 8;
    int frac = note_fixed & 0xff;
    int octave = semi / 12;
    int idx = semi % 12;
    int block = octave - 1;
    if (block < 0) {
        block = 0;
    }
    if (block > 7) {
        block = 7;
    }
    int f0 = note_fnum[idx];
    // Next semitone: wraps to double the octave's first fnum (same pitch, so the
    // block stays fixed and interpolation is well-defined at the octave edge).
    int f1 = idx < 11 ? note_fnum[idx + 1] : note_fnum[0] * 2;
    int fnum = f0 + (((f1 - f0) * frac) >> 8);
    *lo = (uint8_t)(fnum & 0xff);
    *hi = (uint8_t)((block << 2) | ((fnum >> 8) & 0x03));
}

// Total-level (0=loudest, 63=silent) for a carrier given a 0-127 volume.
static uint8_t level_for(const struct genmidi_op* op, int volume)
{
    int base = op->level & 0x3f;
    int tl = 0x3f - (((0x3f - base) * volume) / 127);
    if (tl < 0) {
        tl = 0;
    }
    if (tl > 0x3f) {
        tl = 0x3f;
    }
    return (uint8_t)((op->scale & 0xc0) | (uint8_t)tl);
}

// Combined perceptual 0-127 loudness from note velocity, channel volume and the
// master music volume, run through the perceptual curve so it maps evenly onto
// the OPL's logarithmic level register.
static int voice_volume(int mus_channel, int velocity)
{
    int cv = chan[mus_channel].volume & 0x7f;
    int nv = velocity & 0x7f;
    int mv = music_volume & 0x7f;
    int v = volmap[nv] * (volmap[cv] + 1) / 128;
    v = v * (volmap[mv] + 1) / 128;
    if (v > 127) {
        v = 127;
    }
    return v;
}

static void write_operator(int vi, boolean carrier, const struct genmidi_op* op,
                           int level_byte)
{
    uint8_t off = op_off[vi] + (carrier ? 3 : 0);
    write_reg(0x20 + off, op->tremolo);
    write_reg(0x40 + off, (uint8_t)level_byte);
    write_reg(0x60 + off, op->attack);
    write_reg(0x80 + off, op->sustain);
    write_reg(0xE0 + off, op->waveform);
}

static void set_voice_freq(int vi, boolean key_on)
{
    struct opl_voice* v = &voices[vi];
    int note_fixed = (v->play_note << 8) + v->gv->base_note_offset * 8 +
                     (chan[v->mus_channel].pitchbend - 128) * 4;
    uint8_t lo, hi;
    calc_fnum(note_fixed, &lo, &hi);
    write_reg(0xA0 + vi, lo);
    write_reg(0xB0 + vi, (uint8_t)((key_on ? 0x20 : 0x00) | hi));
}

static int alloc_voice(void)
{
    int oldest = 0;
    for (int i = 0; i < NUM_VOICES; i++) {
        if (!voices[i].used) {
            return i;
        }
        if (voices[i].age < voices[oldest].age) {
            oldest = i;
        }
    }
    // Steal the oldest: key it off first.
    write_reg(0xB0 + oldest, 0x00);
    return oldest;
}

static void note_off(int mus_channel, int note)
{
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].used && voices[i].mus_channel == mus_channel &&
            voices[i].note == note) {
            write_reg(0xB0 + i, 0x00); // key off (release envelope continues)
            voices[i].used = false;
        }
    }
}

static void note_on(int mus_channel, int note, int velocity)
{
    if (genmidi == NULL) {
        return;
    }
    const struct genmidi_instr* in;
    if (mus_channel == MUS_PERCUSSION) {
        if (note < 35 || note > 81) {
            return;
        }
        in = &genmidi[GENMIDI_PERCUSSION_BASE + note - 35];
    } else {
        int prog = chan[mus_channel].instrument;
        if (prog < 0 || prog > 127) {
            prog = 0;
        }
        in = &genmidi[prog];
    }
    const struct genmidi_voice* gv = &in->voices[0];
    int play_note = note;
    if (in->flags & GENMIDI_FLAG_FIXED) {
        play_note = in->fixed_note;
    }

    int vi = alloc_voice();
    struct opl_voice* v = &voices[vi];
    v->used = true;
    v->mus_channel = mus_channel;
    v->note = note;
    v->play_note = play_note;
    v->gv = gv;
    v->additive = (gv->feedback & 0x01) != 0;
    v->age = ++voice_age;

    int vol = voice_volume(mus_channel, velocity);
    // Modulator: patch level, unless additive (both operators reach the output).
    int mod_level = v->additive
                            ? level_for(&gv->mod, vol)
                            : ((gv->mod.scale & 0xc0) | (gv->mod.level & 0x3f));
    write_operator(vi, false, &gv->mod, mod_level);
    write_operator(vi, true, &gv->car, level_for(&gv->car, vol));
    write_reg(0xC0 + vi, gv->feedback | 0x30); // feedback|conn + L/R enable
    set_voice_freq(vi, true);
}

static void channel_notes_off(int mus_channel)
{
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].used && voices[i].mus_channel == mus_channel) {
            write_reg(0xB0 + i, 0x00);
            voices[i].used = false;
        }
    }
}

static void rebend_channel(int mus_channel)
{
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].used && voices[i].mus_channel == mus_channel) {
            set_voice_freq(i, true);
        }
    }
}

static void all_voices_off(void)
{
    for (int i = 0; i < NUM_VOICES; i++) {
        write_reg(0xB0 + i, 0x00);
        voices[i].used = false;
    }
}

static void reset_synth(void)
{
    OPL3_Reset(&chip, OPL_RATE);
    write_reg(0x105, 0x01); // OPL3 mode: sound both stereo channels
    write_reg(0x104, 0x00); // no 4-operator channels
    write_reg(0x01, 0x20);  // waveform select enable
    write_reg(0x08, 0x00);
    write_reg(0xBD, 0x00); // no rhythm mode, no deep AM/VIB
    for (int i = 0; i < NUM_VOICES; i++) {
        voices[i].used = false;
    }
    voice_age = 0;
    for (int i = 0; i < 16; i++) {
        chan[i].instrument = 0;
        chan[i].volume = 127;
        chan[i].velocity = 127;
        chan[i].pitchbend = 128;
    }
}

static int read_varlen(void)
{
    int value = 0;
    for (;;) {
        if (mus_pos >= mus_len) {
            return 0;
        }
        uint8_t b = mus_data[mus_pos++];
        value = value * 128 + (b & 0x7f);
        if (!(b & 0x80)) {
            return value;
        }
    }
}

// Process events until one carries the delay flag; return the delay in ticks,
// or -1 at the score end / data exhaustion.
static int process_events(void)
{
    int guard = 0;
    for (;;) {
        if (mus_pos >= mus_len || ++guard > 20000) {
            return -1;
        }
        uint8_t desc = mus_data[mus_pos++];
        int type = desc & 0x70;
        int ch = desc & 0x0f;
        switch (type) {
        case 0x00: { // release note
            int note = mus_data[mus_pos++] & 0x7f;
            note_off(ch, note);
            break;
        }
        case 0x10: { // play note
            uint8_t k = mus_data[mus_pos++];
            int note = k & 0x7f;
            if (k & 0x80) {
                chan[ch].velocity = mus_data[mus_pos++] & 0x7f;
            }
            note_on(ch, note, chan[ch].velocity);
            break;
        }
        case 0x20: { // pitch wheel
            chan[ch].pitchbend = mus_data[mus_pos++];
            rebend_channel(ch);
            break;
        }
        case 0x30: { // system event (controllers 10-14)
            int ctrl = mus_data[mus_pos++] & 0x7f;
            if (ctrl == 10 || ctrl == 11) { // all sounds off / all notes off
                channel_notes_off(ch);
            }
            break;
        }
        case 0x40: { // change controller
            int ctrl = mus_data[mus_pos++] & 0x7f;
            int val = mus_data[mus_pos++] & 0x7f;
            if (ctrl == 0) {
                chan[ch].instrument = val; // program change
            } else if (ctrl == 3) {
                chan[ch].volume = val; // channel volume
            }
            break;
        }
        case 0x60: // score end
            return -1;
        default:
            break;
        }
        if (desc & 0x80) {
            return read_varlen();
        }
    }
}

static void schedule_delay(int ticks)
{
    long total = (long)ticks * OPL_RATE + tick_frac;
    samples_until_event = total / MUS_RATE;
    tick_frac = total % MUS_RATE;
}

// --- doomgeneric music_module_t hooks ---------------------------------------

static boolean I_JP_InitMusic(void)
{
    load_genmidi();
    return genmidi != NULL; // no GENMIDI -> report no music, game runs silent
}

static void I_JP_ShutdownMusic(void)
{
    playing = false;
    juampi_music_stop();
}

static void I_JP_SetMusicVolume(int volume)
{
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 127) {
        volume = 127;
    }
    music_volume = volume;
}

static void I_JP_PauseMusic(void)
{
    paused = true;
}

static void I_JP_ResumeMusic(void)
{
    paused = false;
}

static void* I_JP_RegisterSong(void* data, int len)
{
    const uint8_t* d = data;
    if (len < 16 || memcmp(d, "MUS\x1a", 4) != 0) {
        return NULL; // only MUS lumps (all of Doom's music) are supported
    }
    struct song* s = malloc(sizeof *s);
    if (s == NULL) {
        return NULL;
    }
    s->data = malloc(len);
    if (s->data == NULL) {
        free(s);
        return NULL;
    }
    memcpy(s->data, d, len);
    s->len = len;
    s->score_start = d[6] | (d[7] << 8);
    return s;
}

static void I_JP_UnRegisterSong(void* handle)
{
    struct song* s = handle;
    if (s == NULL) {
        return;
    }
    if (playing && mus_data == s->data) {
        I_JP_ShutdownMusic();
        mus_data = NULL;
    }
    free(s->data);
    free(s);
}

static void I_JP_PlaySong(void* handle, boolean loop)
{
    struct song* s = handle;
    if (s == NULL) {
        return;
    }
    load_genmidi();
    if (genmidi == NULL) {
        return;
    }
    mus_data = s->data;
    mus_len = s->len;
    mus_score_start = s->score_start;
    mus_pos = mus_score_start;
    looping = loop;
    paused = false;
    samples_until_event = 0;
    tick_frac = 0;
    reset_synth();
    playing = true;
    juampi_music_start();
}

static void I_JP_StopSong(void)
{
    if (!playing) {
        return;
    }
    all_voices_off();
    playing = false;
    juampi_music_stop();
}

static boolean I_JP_MusicIsPlaying(void)
{
    return playing;
}

// Called ~35 Hz by the game (via I_UpdateSound): keep the kernel ring topped up
// by rendering OPL samples up to each event, then processing that event batch.
static void I_JP_PollMusic(void)
{
    if (!playing || paused) {
        return;
    }
    int budget = juampi_music_space();
    if (budget > 8192) {
        budget = 8192; // don't monopolise one Poll
    }
    while (budget > 0) {
        if (samples_until_event <= 0) {
            int ticks = process_events();
            if (ticks < 0) { // score end
                if (!looping) {
                    I_JP_StopSong();
                    return;
                }
                mus_pos = mus_score_start;
                all_voices_off();
                ticks = process_events();
                if (ticks < 0) {
                    I_JP_StopSong();
                    return;
                }
            }
            schedule_delay(ticks);
            continue; // a zero-tick batch just loops back for the next events
        }
        int n = samples_until_event;
        if (n > budget) {
            n = budget;
        }
        if (n > 2048) {
            n = 2048;
        }
        OPL3_GenerateStream(&chip, genbuf, (uint32_t)n);
        int wrote = juampi_music_write(genbuf, n);
        samples_until_event -= wrote;
        budget -= wrote;
        if (wrote < n) {
            break; // ring unexpectedly full; resume next Poll
        }
    }
}

static snddevice_t music_devices[] = {SNDDEVICE_GENMIDI};

music_module_t DG_music_module = {
        music_devices,
        arrlen(music_devices),
        I_JP_InitMusic,
        I_JP_ShutdownMusic,
        I_JP_SetMusicVolume,
        I_JP_PauseMusic,
        I_JP_ResumeMusic,
        I_JP_RegisterSong,
        I_JP_UnRegisterSong,
        I_JP_PlaySong,
        I_JP_StopSong,
        I_JP_MusicIsPlaying,
        I_JP_PollMusic,
};
