// The `audio` library: play synthesized tones through the kernel mixer and
// report the audio backend state. PCM/QOA playback lands here in later
// milestones (audio.play); for now it is tones + introspection.

#include <audio.h>
#include <luadoc.h>

#include "lua.h"
#include "lauxlib.h"

// audio.info() -> present, backend.
static int l_info(lua_State* L)
{
    lua_pushboolean(L, audio_present());
    lua_pushstring(L, audio_backend_name());
    return 2;
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

// audio.stop([voice]) -> stop one voice, or all when omitted.
static int l_stop(lua_State* L)
{
    audio_stop(lua_isnoneornil(L, 1) ? -1 : (int)luaL_checkinteger(L, 1));
    return 0;
}

static const lua_fndoc audiolib[] = {
        {"info", l_info, "Audio backend state.",
         .rets = {{"present", "boolean", "true if an output device is up"},
                  {"backend", "string", "backend name (ac97 / none)"}}},
        {"tone", l_tone, "Play a sine tone through the mixer.",
         .args = {{"freq", "number", "frequency in Hz"},
                  {"ms", "number?", "duration in ms (default 200)"},
                  {"gain", "number?", "0..1 loudness (default 0.5)"}},
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
