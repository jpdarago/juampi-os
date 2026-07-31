// juampiOS sound backend for doomgeneric (DG_sound_module). Each Doom logical
// channel maps to a kernel mixer voice: StartSound loads the DMX lump, hands its
// 8-bit PCM to juampi_audio_play() (the kernel resamples 11 kHz -> 48 kHz and
// mixes), and remembers the voice handle so Stop/IsPlaying can act on that exact
// play. The mixer is fed from the audio completion IRQ, so sound keeps going
// while Doom runs its own loop. Music is not implemented (a null module below).

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "deh_str.h"

#include <juampi.h>
#include <stdio.h>

#define NUM_CHANNELS 16
static int chan_voice[NUM_CHANNELS]; // kernel voice handle per channel, -1 idle
static boolean use_prefix;

// Config knobs the original SDL sfx backend (i_sdlsound.c, not built here)
// defined; I_BindSoundVariables still binds them under FEATURE_SOUND, so provide
// them. Unused: the kernel mixer does its own resampling. Defaults match vanilla.
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static boolean I_JP_InitSound(boolean use_sfx_prefix)
{
    use_prefix = use_sfx_prefix;
    for (int i = 0; i < NUM_CHANNELS; i++) {
        chan_voice[i] = -1;
    }
    return true;
}

static void I_JP_ShutdownSound(void)
{
}

static int I_JP_GetSfxLumpNum(sfxinfo_t* sfx)
{
    char name[16];
    if (use_prefix) {
        snprintf(name, sizeof name, "ds%s", DEH_String(sfx->name));
    } else {
        snprintf(name, sizeof name, "%s", DEH_String(sfx->name));
    }
    return W_GetNumForName(name);
}

static void I_JP_Update(void)
{
}

static void I_JP_UpdateSoundParams(int channel, int vol, int sep)
{
    (void)channel;
    (void)vol;
    (void)sep; // stereo panning not implemented (mono/centered)
}

static int I_JP_StartSound(sfxinfo_t* sfxinfo, int channel, int vol, int sep)
{
    (void)sep;
    if (channel < 0 || channel >= NUM_CHANNELS) {
        return -1;
    }
    int lump = sfxinfo->lumpnum >= 0 ? sfxinfo->lumpnum
                                     : I_JP_GetSfxLumpNum(sfxinfo);
    if (lump < 0) {
        return -1;
    }
    const unsigned char* data = W_CacheLumpNum(lump, PU_STATIC);
    int len = W_LumpLength(lump);
    if (data == NULL || len < 8) {
        return -1;
    }
    // DMX lump: [format u16][rate u16][count u32][u8 samples...], with 16
    // padding samples at each end (vanilla convention).
    int rate = data[2] | (data[3] << 8);
    unsigned count = (unsigned)data[4] | ((unsigned)data[5] << 8) |
                     ((unsigned)data[6] << 16) | ((unsigned)data[7] << 24);
    const unsigned char* samples = data + 8;
    if (8 + count > (unsigned)len) {
        count = (unsigned)len - 8;
    }
    if (count >= 32) {
        samples += 16;
        count -= 32;
    }

    if (chan_voice[channel] >= 0) {
        juampi_audio_stop(chan_voice[channel]); // reuse this channel
    }
    chan_voice[channel] =
            juampi_audio_play(samples, (int)count, rate ? rate : 11025, vol);
    return channel;
}

static void I_JP_StopSound(int channel)
{
    if (channel < 0 || channel >= NUM_CHANNELS) {
        return;
    }
    if (chan_voice[channel] >= 0) {
        juampi_audio_stop(chan_voice[channel]);
        chan_voice[channel] = -1;
    }
}

static boolean I_JP_SoundIsPlaying(int channel)
{
    if (channel < 0 || channel >= NUM_CHANNELS || chan_voice[channel] < 0) {
        return false;
    }
    return juampi_audio_playing(chan_voice[channel]) != 0;
}

static void I_JP_CacheSounds(sfxinfo_t* sounds, int num_sounds)
{
    (void)sounds;
    (void)num_sounds;
}

static snddevice_t sound_devices[] = {
        SNDDEVICE_SB,          SNDDEVICE_PAS,        SNDDEVICE_GUS,
        SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
        sound_devices,
        arrlen(sound_devices),
        I_JP_InitSound,
        I_JP_ShutdownSound,
        I_JP_GetSfxLumpNum,
        I_JP_Update,
        I_JP_UpdateSoundParams,
        I_JP_StartSound,
        I_JP_StopSound,
        I_JP_SoundIsPlaying,
        I_JP_CacheSounds,
};

// DG_music_module (OPL FM music) lives in i_juampimusic.c.
