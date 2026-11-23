/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Playback Kit** - `use("/Kosmos/Kits/playback")` - a song's or a
 * film's sound, played on a thread of its own in the audio band
 * (`docs/playback.md`, step 3).
 *
 *   local playback = use("/Kosmos/Kits/playback")
 *   local p, why = playback.open{ kind = "wav", rate = 44100,
 *                                 channels = 2, bits = 16 }  -- or "mp3"
 *   p:seek(stream.ring)   the stream's ring to play into, and everything
 *                         before it forgotten: the first start and every
 *                         seek. Returns once the thread has let go of the
 *                         old ring, so the caller may close its stream.
 *   p:room()              bytes one frame may be and still go now
 *   p:frame(at, bytes)    a frame from an address in this process: true,
 *                         or false when the input ring is full - an MP3's
 *                         or a WAV's bytes in pieces of 16 KB at most.
 *                         A film's sound, AAC or Opus or any other, comes
 *                         as a WAV's PCM from the Media Kit (`dec:sound()`);
 *                         AAC of its own went in 0.12.97
 *   p:ended()             no frame will come after these
 *   p:play()  p:pause()
 *   p:state(t)            what is heard, into `t`
 *   p:close()
 *
 * **Decoded sound, a queue of songs** (`docs/player.md` P3), for a feeder
 * that decodes through the Media Kit:
 *
 *   local p = playback.open{ kind = "pcm", format = "s32", rate = 96000,
 *                            channels = 2, seconds = 8 }  -- the ring's size
 *   local bits = playback.stream_bits("s32", 24)   -- 24: open the stream so
 *   p:seek(stream.ring, 96000, bits, 2)            -- the stream's format
 *   p:song{ song = 7, format = "s32", rate = 96000, channels = 2, bits = 24,
 *           delay = 0, length = 0, first = 0, from = 0 }   -- true, or false: full
 *   p:frame(at, bytes)    whole frames of the song, after its `song`
 *   p:continue(ring, rate, bits, channels)   after `state.change`: a stream
 *                         at the next song's format, and on from there
 *
 * `state` adds `ring`, `ring_frames`, `marks`, the newest mark as `song`,
 * `song_ring`, `song_at`, `song_from` and the one before as `previous`,
 * `previous_ring`, `previous_at`, `previous_from`; `change` with
 * `change_rate`, `change_bits`, `change_channels`; and `untrusted`.
 *
 * **Why a thread.** A song and a film's sound were fed from the program's
 * own loop, in Lua (`media.feed`), so the sound was exactly as punctual as
 * the window: held away longer than its ring by a picture's decode, a
 * collection or a busy machine, it left the device nothing (`roadmap.md`
 * 6zw). Here the window only reads the file - which a thread in the audio
 * band may not, since it waits on no server - and hands over compressed
 * frames, seconds of them at once; the thread decodes, converts and writes
 * straight into the stream's ring, as the Synth Kit's does.
 *
 * **The player is `playback_player.c`** (`playback.h`), its thread and its
 * rings; this is its Lua face and nothing else, so a program in Lua and
 * JetSki's web process in C play through the same code.
 *
 * Volume is not here: it is the stream's, set at the audio server
 * (`audio.set`), as it always was.
 *
 * Everything is allocated at `open` and freed at `close`; the thread's pass
 * allocates nothing (`playback_core.h`).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "audioring.h"
#include "playback.h"
#include "playback_core.h"

#define PLAYBACK_MT "kosmos.playback"

struct handle {
    struct playback *p;
};

static struct playback *check(lua_State *L)
{
    struct handle *h = (struct handle *)luaL_checkudata(L, 1, PLAYBACK_MT);

    if (h->p == NULL) {
        luaL_error(L, "this player is closed");
    }

    return h->p;
}

static enum pb_kind kind_named(lua_State *L, const char *name)
{
    if (strcmp(name, "wav") == 0) return PB_WAV;
    if (strcmp(name, "mp3") == 0) return PB_MP3;
    if (strcmp(name, "pcm") == 0) return PB_PCM;
    luaL_error(L, "a kind of sound is \"wav\", \"mp3\" or \"pcm\", not \"%s\"", name);
    return PB_WAV;
}

static int format_named(lua_State *L, const char *name)
{
    if (strcmp(name, "s16") == 0) return PLAYBACK_PCM_S16;
    if (strcmp(name, "s32") == 0) return PLAYBACK_PCM_S32;
    if (strcmp(name, "f32") == 0) return PLAYBACK_PCM_F32;
    luaL_error(L, "a format of samples is \"s16\", \"s32\" or \"f32\", not \"%s\"", name);
    return 0;
}

/* The table at `index`'s `format`, named. */
static int field_format(lua_State *L, int index)
{
    int f;

    lua_getfield(L, index, "format");
    f = format_named(L, luaL_checkstring(L, -1));
    lua_pop(L, 1);
    return f;
}

static lua_Integer field_integer(lua_State *L, const char *key)
{
    lua_Integer v;

    lua_getfield(L, 1, key);
    v = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    return v;
}

/*
 * playback.open{ kind, rate, channels, bits } - a player, its
 * thread started and paused, with no ring to play into until `seek`.
 */
static int l_open(lua_State *L)
{
    const char *why = NULL;
    enum pb_kind kind;
    struct handle *h;
    struct playback *p;

    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "kind");
    kind = kind_named(L, luaL_checkstring(L, -1));
    lua_pop(L, 1);

    h = (struct handle *)lua_newuserdatauv(L, sizeof *h, 0);
    h->p = NULL;
    luaL_setmetatable(L, PLAYBACK_MT);

    if (kind == PB_PCM) {
        p = playback_open_pcm(field_format(L, 1), (long)field_integer(L, "rate"),
                              (long)field_integer(L, "channels"),
                              (unsigned)field_integer(L, "seconds"), &why);
    } else {
        p = playback_open(kind, (long)field_integer(L, "rate"),
                          (long)field_integer(L, "channels"), (long)field_integer(L, "bits"),
                          NULL, 0, &why);
    }

    if (p == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }

    h->p = p;
    return 1;
}

/*
 * p:seek(ring) - play into this ring from now, everything before forgotten.
 * Waits until the thread has taken it - a tick or two - because the caller
 * closes the old stream next, and the thread must not be writing into it.
 */
static int l_seek(lua_State *L)
{
    struct playback *p = check(L);
    struct audio_ring *r = (struct audio_ring *)(uintptr_t)luaL_checkinteger(L, 2);
    long rate = (long)luaL_optinteger(L, 3, 0);
    long bits = (long)luaL_optinteger(L, 4, 0);
    long channels = (long)luaL_optinteger(L, 5, 0);

    luaL_argcheck(L, audio_ring_valid(r), 2, "not a stream's ring");

    if (!playback_seek_format(p, r, rate, bits, channels)) {
        return luaL_error(L, "the player did not take the seek: a ring not of its "
                             "format (%d Hz, %d bits, %d channels), or its thread is late",
                          (int)rate, (int)bits, (int)channels);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* p:continue(ring, rate, bits, channels) - after `change`, a stream at the new format. */
static int l_continue(lua_State *L)
{
    struct playback *p = check(L);
    struct audio_ring *r = (struct audio_ring *)(uintptr_t)luaL_checkinteger(L, 2);

    luaL_argcheck(L, audio_ring_valid(r), 2, "not a stream's ring");

    if (!playback_continue(p, r, (long)luaL_checkinteger(L, 3), (long)luaL_checkinteger(L, 4),
                           (long)luaL_checkinteger(L, 5))) {
        return luaL_error(L, "the player did not take the ring: not of its format, "
                             "or its thread is late");
    }

    lua_pushboolean(L, 1);
    return 1;
}

static uint64_t field_count(lua_State *L, const char *key)
{
    lua_Integer v = field_integer(L, key);

    if (v < 0) {
        luaL_error(L, "a song's %s is a count of frames, not %d", key, (int)v);
    }

    return (uint64_t)v;
}

/* p:song{ song, format, rate, channels, bits, delay, length, first, from } */
static int l_song(lua_State *L)
{
    struct playback *p = check(L);
    struct playback_song s;
    const char *why = NULL;
    int r;

    luaL_checktype(L, 2, LUA_TTABLE);
    lua_settop(L, 2);
    lua_remove(L, 1);                   /* the table at 1, for field_integer */

    memset(&s, 0, sizeof s);
    s.song = (uint32_t)field_integer(L, "song");
    s.format = (uint32_t)field_format(L, 1);
    s.rate = (uint32_t)field_integer(L, "rate");
    s.channels = (uint32_t)field_integer(L, "channels");
    s.bits = (uint32_t)field_integer(L, "bits");
    s.delay = field_count(L, "delay");
    s.length = field_count(L, "length");
    s.first = field_count(L, "first");
    s.from = field_count(L, "from");

    r = playback_song(p, &s, &why);

    if (r < 0) {
        return luaL_error(L, "%s", why);
    }

    lua_pushboolean(L, r);
    return 1;
}

/* playback.stream_bits(format, source_bits) - the bits a song's stream is opened at. */
static int l_stream_bits(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)playback_stream_bits(format_named(L, luaL_checkstring(L, 1)),
                                                         (long)luaL_optinteger(L, 2, 0)));
    return 1;
}

static int l_play(lua_State *L)
{
    lua_pushboolean(L, playback_play(check(L)));
    return 1;
}

static int l_pause(lua_State *L)
{
    lua_pushboolean(L, playback_pause(check(L)));
    return 1;
}

static int l_room(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)playback_room(check(L)));
    return 1;
}

static int put(lua_State *L, struct playback *p, const void *bytes, size_t n)
{
    const char *why = NULL;
    int r = playback_frame(p, bytes, n, &why);

    if (r < 0) {
        return luaL_error(L, "%s (%d bytes; %d at most)", why, (int)n,
                          (int)playback_piece(p));
    }

    lua_pushboolean(L, r);
    return 1;
}

/* p:frame(at, bytes) - a frame from an address in this process. */
static int l_frame(lua_State *L)
{
    struct playback *p = check(L);
    lua_Integer at = luaL_checkinteger(L, 2);
    lua_Integer n = luaL_checkinteger(L, 3);

    if (at == 0 || n < 0) {
        return luaL_error(L, "a frame is an address and a length");
    }

    return put(L, p, (const void *)(uintptr_t)at, (size_t)n);
}

/* p:ended() - an empty record that says so. False while the ring is full. */
static int l_ended(lua_State *L)
{
    lua_pushboolean(L, playback_ended(check(L)));
    return 1;
}

static void set_integer(lua_State *L, const char *key, lua_Integer v)
{
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}

static void set_boolean(lua_State *L, const char *key, int v)
{
    lua_pushboolean(L, v);
    lua_setfield(L, -2, key);
}

/* p:state([t]) - what is heard (`struct playback_state`). */
static int l_state(lua_State *L)
{
    struct playback *p = check(L);
    struct playback_state s;

    if (lua_istable(L, 2)) {
        lua_settop(L, 2);
    } else {
        lua_settop(L, 1);
        lua_newtable(L);
    }

    playback_state(p, &s);
    set_integer(L, "periods", (lua_Integer)s.periods);
    set_integer(L, "frames_in", (lua_Integer)s.frames_in);
    set_integer(L, "frames_out", (lua_Integer)s.frames_out);
    set_integer(L, "dry", (lua_Integer)s.dry);
    set_integer(L, "refused", (lua_Integer)s.refused);
    set_boolean(L, "finished", s.finished);
    set_boolean(L, "playing", s.playing);
    set_integer(L, "worst_pass", (lua_Integer)s.worst_pass);
    set_boolean(L, "audio_band", s.audio_band);
    lua_pushstring(L, s.why);
    lua_setfield(L, -2, "why");

    set_integer(L, "ring", (lua_Integer)s.ring);
    set_integer(L, "ring_frames", (lua_Integer)s.ring_frames);
    set_integer(L, "marks", (lua_Integer)s.marks);
    set_integer(L, "song", s.marks > 0 ? (lua_Integer)s.mark[0].song : -1);
    set_integer(L, "song_ring", (lua_Integer)s.mark[0].ring);
    set_integer(L, "song_at", (lua_Integer)s.mark[0].at);
    set_integer(L, "song_from", (lua_Integer)s.mark[0].from);
    set_integer(L, "previous", s.marks > 1 ? (lua_Integer)s.mark[1].song : -1);
    set_integer(L, "previous_ring", (lua_Integer)s.mark[1].ring);
    set_integer(L, "previous_at", (lua_Integer)s.mark[1].at);
    set_integer(L, "previous_from", (lua_Integer)s.mark[1].from);
    set_boolean(L, "change", s.change);
    set_integer(L, "change_rate", (lua_Integer)s.change_rate);
    set_integer(L, "change_bits", (lua_Integer)s.change_bits);
    set_integer(L, "change_channels", (lua_Integer)s.change_channels);
    set_integer(L, "untrusted", (lua_Integer)s.untrusted);
    return 1;
}

/* Also the collector's, so a player nobody closed still stops its thread. */
static int l_close(lua_State *L)
{
    struct handle *h = (struct handle *)luaL_checkudata(L, 1, PLAYBACK_MT);

    playback_close(h->p);
    h->p = NULL;
    return 0;
}

void kosmos_playback_kit(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "seek",         l_seek },
        { "continue",     l_continue },
        { "song",         l_song },
        { "play",         l_play },
        { "pause",        l_pause },
        { "room",         l_room },
        { "frame",        l_frame },
        { "ended",        l_ended },
        { "state",        l_state },
        { "close",        l_close },
        { NULL, NULL }
    };

    static const luaL_Reg api[] = {
        { "open", l_open },
        { "stream_bits", l_stream_bits },
        { NULL, NULL }
    };

    if (luaL_newmetatable(L, PLAYBACK_MT)) {
        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_close);
        lua_setfield(L, -2, "__gc");
    }

    lua_pop(L, 1);
    luaL_newlib(L, api);
}
