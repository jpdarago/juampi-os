// The `audio` library: play synthesized tones through the kernel mixer and
// report the audio backend state. PCM/QOA playback lands here in later
// milestones (audio.play); for now it is tones + introspection.

#include <audio.h>
#include <qoa.h>
#include <kmodule.h>
#include <ext2.h>
#include <memory.h>
#include <luadoc.h>

#include "lua.h"
#include "lauxlib.h"

// audio.info() -> present, backend, irq_driven, irq_count.
static int l_info(lua_State* L)
{
    lua_pushboolean(L, audio_present());
    lua_pushstring(L, audio_backend_name());
    lua_pushboolean(L, audio_irq_driven());
    lua_pushinteger(L, (lua_Integer)audio_irq_count());
    return 4;
}

// audio.tone(freq [,ms=200 [,gain=0.5]]) -> voice id | nil.
static int l_tone(lua_State* L)
{
    lua_Integer freq = luaL_checkinteger(L, 1);
    lua_Integer ms = luaL_optinteger(L, 2, 200);
    lua_Number gain = luaL_optnumber(L, 3, 0.5);
    if (freq <= 0) {
        return luaL_error(L, "audio.tone: freq must be positive");
    }
    int v = audio_tone((uint32_t)freq, (uint32_t)(ms < 0 ? 0 : ms), (float)gain);
    if (v < 0) {
        lua_pushnil(L);
        lua_pushstring(L, audio_present() ? "no free voice" : "no audio device");
        return 2;
    }
    lua_pushinteger(L, v);
    return 1;
}

// audio.play_pcm(data, rate [,channels=1 [,loop=false [,gain=0.7]]]) -> voice.
// `data` is interleaved little-endian signed-16 PCM (e.g. from string.pack).
static int l_play_pcm(lua_State* L)
{
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    lua_Integer rate = luaL_checkinteger(L, 2);
    lua_Integer ch = luaL_optinteger(L, 3, 1);
    bool loop = lua_toboolean(L, 4);
    lua_Number gain = luaL_optnumber(L, 5, 0.7);
    if (rate <= 0 || (ch != 1 && ch != 2)) {
        return luaL_error(L, "audio.play_pcm: bad rate/channels");
    }
    size_t frame_bytes = (size_t)ch * sizeof(int16_t);
    if (len == 0 || len % frame_bytes != 0) {
        return luaL_error(L, "audio.play_pcm: data not whole s16 frames");
    }
    int v = audio_play_pcm((const int16_t*)data, (uint32_t)(len / frame_bytes),
                           (uint32_t)rate, (uint8_t)ch, loop, (float)gain);
    if (v < 0) {
        lua_pushnil(L);
        lua_pushstring(L, audio_present() ? "no free voice" : "no audio device");
        return 2;
    }
    lua_pushinteger(L, v);
    return 1;
}

// audio.play(name [,loop [,gain]]) -> voice. Load a QOA file (a Limine module,
// then the ext2 disk), decode it, and play it resampled to the mixer rate.
static int l_play(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    bool loop = lua_toboolean(L, 2);
    lua_Number gain = luaL_optnumber(L, 3, 0.8);

    size_t size = 0;
    void* owned = NULL;                              // ext2 buffer to free
    const void* data = kmodule_find(name, &size);    // built-in module?
    if (data == NULL) {
        owned = ext2_read_path(name, &size);         // else the data disk
        data = owned;
    }
    if (data == NULL) {
        return luaL_error(L, "audio.play: no such file: %s", name);
    }

    struct qoa_desc d;
    int16_t* pcm = qoa_decode(&heap_default()->base, data, size, &d);
    if (owned != NULL) {
        ext2_free(owned);
    }
    if (pcm == NULL || d.samples == 0) {
        if (pcm != NULL) {
            heap_free(heap_default(), pcm);
        }
        return luaL_error(L, "audio.play: not a valid QOA file: %s", name);
    }
    // audio_play_pcm copies into a voice-owned buffer, so free the decode here.
    int v = audio_play_pcm(pcm, d.samples, d.samplerate, (uint8_t)d.channels,
                           loop, (float)gain);
    heap_free(heap_default(), pcm);
    if (v < 0) {
        lua_pushnil(L);
        lua_pushstring(L, audio_present() ? "no free voice" : "no audio device");
        return 2;
    }
    lua_pushinteger(L, v);
    return 1;
}

// audio.stop([voice]) -> stop one voice, or all when omitted.
static int l_stop(lua_State* L)
{
    audio_stop(lua_isnoneornil(L, 1) ? -1 : (int)luaL_checkinteger(L, 1));
    return 0;
}

static const struct lua_fndoc audiolib[] = {
        {"info", l_info, "Audio backend + interrupt state.",
         .rets = {{"present", "boolean", "true if an output device is up"},
                  {"backend", "string", "backend name (ac97 / none)"},
                  {"irq", "boolean", "true when ISR-fed (else polled)"},
                  {"irqs", "number", "completion interrupts taken"}}},
        {"tone", l_tone, "Play a sine tone through the mixer.",
         .args = {{"freq", "number", "frequency in Hz"},
                  {"ms", "number?", "duration in ms (default 200)"},
                  {"gain", "number?", "0..1 loudness (default 0.5)"}},
         .rets = {{"voice", "number?", "voice id, or nil on error"},
                  {"err", "string?", "message when voice is nil"}}},
        {"play_pcm", l_play_pcm,
         "Play interleaved s16 PCM (resampled to the mixer rate).",
         .args = {{"data", "string", "little-endian s16 samples"},
                  {"rate", "number", "source sample rate in Hz"},
                  {"channels", "number?", "1 or 2 (default 1)"},
                  {"loop", "boolean?", "repeat until stopped"},
                  {"gain", "number?", "0..1 loudness (default 0.7)"}},
         .rets = {{"voice", "number?", "voice id, or nil on error"},
                  {"err", "string?", "message when voice is nil"}}},
        {"play", l_play, "Decode and play a QOA audio file (module or disk).",
         .args = {{"name", "string", "QOA file name"},
                  {"loop", "boolean?", "repeat until stopped"},
                  {"gain", "number?", "0..1 loudness (default 0.8)"}},
         .rets = {{"voice", "number?", "voice id, or nil on error"},
                  {"err", "string?", "message when voice is nil"}}},
        {"stop", l_stop, "Stop a voice, or all voices.",
         .args = {{"voice", "number?", "voice id; omit to stop all"}}},
        {0},
};

int luaopen_audio(lua_State* L)
{
    luadoc_newlib(L, audiolib);
    return 1;
}
