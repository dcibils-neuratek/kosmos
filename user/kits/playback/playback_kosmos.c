/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Playback Kit** - `use("/Kosmos/Kits/playback")` - a song's or a
 * film's sound, played on a thread of its own in the audio band
 * (`docs/playback.md`, step 3).
 *
 *   local playback = use("/Kosmos/Kits/playback")
 *   local p, why = playback.open{ kind = "aac", config = esds,
 *                                 rate = 44100, channels = 2 }
 *                                       -- or "mp3"; "wav" with bits too
 *   p:seek(stream.ring)   the stream's ring to play into, and everything
 *                         before it forgotten: the first start and every
 *                         seek. Returns once the thread has let go of the
 *                         old ring, so the caller may close its stream.
 *   p:room()              bytes one frame may be and still go now
 *   p:frame(at, bytes)    a frame from an address in this process: true,
 *                         or false when the input ring is full
 *   p:frame_string(s)     the same from a string
 *   p:ended()             no frame will come after these
 *   p:play()  p:pause()
 *   p:state(t)            what is heard, into `t`
 *   p:close()
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
 * **What crosses between the two threads, and each one way:**
 *
 * - frames, window to thread, in the input ring (`playback_core.h`);
 * - commands - play, pause, a seek with its new ring - in a ring of fixed
 *   records, the window writing and the thread reading, and `applied` back
 *   so that a seek can know the old ring is no longer touched;
 * - what is heard, published after each pass under a sequence number and
 *   read by the window as a copy.
 *
 * Volume is not here: it is the stream's, set at the audio server
 * (`audio.set`), as it always was.
 *
 * Everything is allocated at `open` and freed at `close`; the thread's pass
 * allocates nothing (`playback_core.h`).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "audioring.h"
#include "kosmos.h"
#include "playback_core.h"

#define PLAYBACK_MT "kosmos.playback"

/*
 * **One megabyte of compressed sound**: a minute of a 128 kb/s song, half a
 * minute of most films' - so the window may be away for seconds and nothing
 * is heard (`docs/playback.md`, *The two rings*).
 */
#define INPUT_BYTES (1u << 20)

/* ------------------------------------------------------------ commands */

enum cmd_kind { CMD_PLAY, CMD_PAUSE, CMD_SEEK };

struct cmd {
    enum cmd_kind kind;
    uint32_t generation;
    struct audio_ring *ring;
};

#define CMDS 64u

/* What the thread publishes, and the window copies. */
struct heard {
    struct pb_heard core;
    uint32_t generation;
    unsigned long worst_pass;           /* counter ticks between two passes */
    bool     playing;
    char     why[96];
};

struct player {
    struct pb core;                     /* the thread's, after `open` */
    struct pb_input *in;
    struct audio_ring *ring;            /* the thread's: where it writes */
    bool playing;                       /* the thread's */

    /* The window's. */
    uint32_t generation;
    bool ended;
    bool closed;

    struct cmd cmds[CMDS];
    uint32_t cmd_head;                  /* written by the window */
    uint32_t cmd_tail;                  /* written by the thread */
    uint32_t applied;                   /* the thread's: commands done */

    int quit;
    long thread;
    bool audio_band;

    /* The thread's, between passes. */
    unsigned long last_pass;
    unsigned long worst_pass;
    unsigned long last_pass_at;         /* read straight by `state` */

    uint32_t heard_seq;
    struct heard heard;
};

struct handle {
    struct player *p;
};

/* ------------------------------------------------------------ the thread */

static void take_commands(struct player *pl)
{
    uint32_t head = __atomic_load_n(&pl->cmd_head, __ATOMIC_ACQUIRE);

    while (pl->cmd_tail != head) {
        const struct cmd *c = &pl->cmds[pl->cmd_tail % CMDS];

        switch (c->kind) {
        case CMD_PLAY:
            if (!pl->playing) {
                pl->last_pass = 0;      /* away is counted while playing */
            }

            pl->playing = true;
            break;

        case CMD_PAUSE:
            pl->playing = false;
            break;

        case CMD_SEEK:
            pb_restart(&pl->core, c->generation);
            pl->ring = c->ring;
            break;
        }

        __atomic_store_n(&pl->cmd_tail, pl->cmd_tail + 1, __ATOMIC_RELEASE);
    }

    /* After the old ring was let go, and not before: a seek waits on this. */
    __atomic_store_n(&pl->applied, pl->cmd_tail, __ATOMIC_RELEASE);
}

static void publish(struct player *pl)
{
    uint32_t seq = pl->heard_seq;

    __atomic_store_n(&pl->heard_seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    pl->heard.core = pl->core.heard;
    pl->heard.generation = pl->core.generation;
    pl->heard.worst_pass = pl->worst_pass;
    pl->heard.playing = pl->playing;

    if (pl->core.why != NULL) {
        strncpy(pl->heard.why, pl->core.why, sizeof pl->heard.why - 1);
        pl->heard.why[sizeof pl->heard.why - 1] = '\0';
    } else {
        pl->heard.why[0] = '\0';
    }

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&pl->heard_seq, seq + 2, __ATOMIC_RELEASE);
}

static void playback_main(unsigned long arg)
{
    struct player *pl = (struct player *)(uintptr_t)arg;

    while (!__atomic_load_n(&pl->quit, __ATOMIC_ACQUIRE)) {
        unsigned long now = kosmos_ticks();

        /* The longest this thread has been away between two passes while
         * playing: a sleep of one tick, and whatever held it off. */
        if (pl->playing && pl->last_pass != 0 && now - pl->last_pass > pl->worst_pass) {
            pl->worst_pass = now - pl->last_pass;
        }

        pl->last_pass = now;
        __atomic_store_n(&pl->last_pass_at, now, __ATOMIC_RELAXED);

        take_commands(pl);

        if (pl->playing && pl->ring != NULL) {
            (void)pb_pass(&pl->core, pl->in, pl->ring);
        }

        publish(pl);

        /*
         * One tick, 4 ms at 250 Hz, against a film's ring of 186 ms and a
         * song's of 46: every pass fills the ring, so a pass a tick keeps it
         * full with room to spare, and the thread is asleep - which is what
         * keeps it inside the audio band's budget.
         */
        kosmos_sleep(1);
    }

    /* A thread that returned from its entry would return to nothing. */
    kosmos_thread_exit(0);
}

/* ------------------------------------------------------------ the window's */

static struct player *check(lua_State *L)
{
    struct handle *h = (struct handle *)luaL_checkudata(L, 1, PLAYBACK_MT);

    if (h->p == NULL) {
        luaL_error(L, "this player is closed");
    }

    return h->p;
}

static bool post(struct player *pl, enum cmd_kind kind, uint32_t generation,
                 struct audio_ring *ring)
{
    uint32_t tail = __atomic_load_n(&pl->cmd_tail, __ATOMIC_ACQUIRE);

    if (pl->cmd_head - tail >= CMDS) {
        return false;                   /* the thread is far behind */
    }

    pl->cmds[pl->cmd_head % CMDS] = (struct cmd){ kind, generation, ring };
    __atomic_store_n(&pl->cmd_head, pl->cmd_head + 1, __ATOMIC_RELEASE);
    return true;
}

static enum pb_kind kind_named(lua_State *L, const char *name)
{
    if (strcmp(name, "wav") == 0) return PB_WAV;
    if (strcmp(name, "mp3") == 0) return PB_MP3;
    if (strcmp(name, "aac") == 0) return PB_AAC;

    luaL_error(L, "a kind of sound is \"wav\", \"mp3\" or \"aac\", not \"%s\"", name);
    return PB_WAV;
}

static lua_Integer field_integer(lua_State *L, const char *key)
{
    lua_Integer v;

    lua_getfield(L, 1, key);
    v = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    return v;
}

static void player_free(struct player *pl)
{
    pb_close(&pl->core);
    free(pl->in);
    free(pl);
}

/*
 * playback.open{ kind, config, rate, channels, bits } - a player, its
 * thread started and paused, with no ring to play into until `seek`.
 */
static int l_open(lua_State *L)
{
    struct player *pl;
    struct handle *h;
    const char *config = NULL;
    size_t config_n = 0;
    enum pb_kind kind;

    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "kind");
    kind = kind_named(L, luaL_checkstring(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 1, "config");
    if (!lua_isnil(L, -1)) {
        config = luaL_checklstring(L, -1, &config_n);
    }

    h = (struct handle *)lua_newuserdatauv(L, sizeof *h, 0);
    h->p = NULL;
    luaL_setmetatable(L, PLAYBACK_MT);

    pl = (struct player *)calloc(1, sizeof *pl);

    if (pl == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "no memory for a player");
        return 2;
    }

    pl->in = (struct pb_input *)malloc(INPUT_BYTES);

    if (pl->in == NULL || !pb_input_init(pl->in, INPUT_BYTES)) {
        free(pl->in);
        free(pl);
        lua_pushnil(L);
        lua_pushstring(L, "no memory for the player's input ring");
        return 2;
    }

    if (pb_open(&pl->core, kind, (long)field_integer(L, "rate"),
                (long)field_integer(L, "channels"), (long)field_integer(L, "bits"),
                (const uint8_t *)config, config_n) != 0) {
        const char *why = pl->core.why ? pl->core.why : "this sound cannot be played";

        lua_pop(L, 1);                  /* the handle, never to be used */
        lua_pushnil(L);
        lua_pushstring(L, why);
        player_free(pl);
        return 2;
    }

    /*
     * In the audio band from its first instruction, if this program may put
     * a thread there (`kosmos: needs audio`); at NORMAL otherwise, as the
     * Synth Kit falls back, and `state().audio_band` says which.
     */
    pl->thread = kosmos_thread_start_audio(playback_main, (unsigned long)(uintptr_t)pl);
    pl->audio_band = pl->thread >= 0;

    if (pl->thread < 0) {
        pl->thread = kosmos_thread_start(playback_main, (unsigned long)(uintptr_t)pl);
    }

    if (pl->thread < 0) {
        long code = pl->thread;

        player_free(pl);
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushfstring(L, "no thread to play on (%d)", (int)code);
        return 2;
    }

    h->p = pl;
    return 1;
}

/*
 * p:seek(ring) - play into this ring from now, everything before forgotten.
 * Waits until the thread has taken it - a tick or two - because the caller
 * closes the old stream next, and the thread must not be writing into it.
 */
static int l_seek(lua_State *L)
{
    struct player *pl = check(L);
    struct audio_ring *r = (struct audio_ring *)(uintptr_t)luaL_checkinteger(L, 2);
    uint32_t want;

    luaL_argcheck(L, audio_ring_valid(r) && r->period_bytes % 4u == 0
                     && r->period_bytes <= sizeof pl->core.period, 2,
                  "not a ring of 16-bit stereo periods it can fill");

    pl->generation = (pl->generation + 1) & 0xffffu;
    pl->ended = false;

    if (!post(pl, CMD_SEEK, pl->generation, r)) {
        return luaL_error(L, "the player's thread is not taking commands");
    }

    want = pl->cmd_head;

    /* Bounded: a thread a second late is a thread that has stopped. */
    for (int i = 0; i < 250; i++) {
        if ((int32_t)(__atomic_load_n(&pl->applied, __ATOMIC_ACQUIRE) - want) >= 0) {
            lua_pushboolean(L, 1);
            return 1;
        }

        kosmos_sleep(1);
    }

    return luaL_error(L, "the player's thread did not take the seek");
}

static int l_play(lua_State *L)
{
    lua_pushboolean(L, post(check(L), CMD_PLAY, 0, NULL));
    return 1;
}

static int l_pause(lua_State *L)
{
    lua_pushboolean(L, post(check(L), CMD_PAUSE, 0, NULL));
    return 1;
}

static int l_room(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)pb_input_room(check(L)->in));
    return 1;
}

static int put(lua_State *L, struct player *pl, const void *bytes, size_t n,
               uint32_t flags)
{
    if (n > pl->in->size / 2) {
        return luaL_error(L, "a frame of %d bytes is larger than the player takes",
                          (int)n);
    }

    lua_pushboolean(L, pb_input_put(pl->in, bytes, (uint32_t)n,
                                    flags | (pl->generation << 16)));
    return 1;
}

/* p:frame(at, bytes) - a frame from an address in this process. */
static int l_frame(lua_State *L)
{
    struct player *pl = check(L);
    lua_Integer at = luaL_checkinteger(L, 2);
    lua_Integer n = luaL_checkinteger(L, 3);

    if (at == 0 || n < 0) {
        return luaL_error(L, "a frame is an address and a length");
    }

    if (pl->ended) {
        return luaL_error(L, "a frame after the end");
    }

    return put(L, pl, (const void *)(uintptr_t)at, (size_t)n, 0);
}

static int l_frame_string(lua_State *L)
{
    struct player *pl = check(L);
    size_t n;
    const char *s = luaL_checklstring(L, 2, &n);

    if (pl->ended) {
        return luaL_error(L, "a frame after the end");
    }

    return put(L, pl, s, n, 0);
}

/* p:ended() - an empty record that says so. False while the ring is full. */
static int l_ended(lua_State *L)
{
    struct player *pl = check(L);

    if (pl->ended) {
        lua_pushboolean(L, 1);
        return 1;
    }

    if (!pb_input_put(pl->in, NULL, 0, PB_END | (pl->generation << 16))) {
        lua_pushboolean(L, 0);
        return 1;
    }

    pl->ended = true;
    lua_pushboolean(L, 1);
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

/*
 * p:state([t]) - what is heard, copied under the sequence number: periods
 * written, frames decoded and converted, how often the input ran dry, the
 * frames that would not decode, whether it has all been written, and the
 * longest the thread was away between two passes while playing - counted up
 * to now as well, so a thread held off for good is seen.
 */
static int l_state(lua_State *L)
{
    struct player *pl = check(L);
    struct heard copy;
    uint32_t seq;

    if (lua_istable(L, 2)) {
        lua_settop(L, 2);
    } else {
        lua_settop(L, 1);
        lua_newtable(L);
    }

    for (int tries = 0; tries < 1000; tries++) {
        seq = __atomic_load_n(&pl->heard_seq, __ATOMIC_ACQUIRE);

        if (seq & 1u) {
            continue;
        }

        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        memcpy(&copy, &pl->heard, sizeof copy);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        if (__atomic_load_n(&pl->heard_seq, __ATOMIC_ACQUIRE) == seq) {
            break;
        }
    }

    unsigned long away = copy.worst_pass;
    unsigned long last = __atomic_load_n(&pl->last_pass_at, __ATOMIC_RELAXED);

    if (copy.playing && last != 0 && kosmos_ticks() - last > away) {
        away = kosmos_ticks() - last;
    }

    set_integer(L, "periods", (lua_Integer)copy.core.periods);
    set_integer(L, "frames_in", (lua_Integer)copy.core.frames_in);
    set_integer(L, "frames_out", (lua_Integer)copy.core.frames_out);
    set_integer(L, "dry", (lua_Integer)copy.core.dry);
    set_integer(L, "refused", (lua_Integer)copy.core.refused);
    set_boolean(L, "finished", copy.core.finished
                               && copy.generation == pl->generation);
    set_boolean(L, "playing", copy.playing);
    set_integer(L, "worst_pass", (lua_Integer)away);
    set_boolean(L, "audio_band", pl->audio_band);

    lua_pushstring(L, copy.why);
    lua_setfield(L, -2, "why");
    return 1;
}

/* Also the collector's, so a player nobody closed still stops its thread. */
static int l_close(lua_State *L)
{
    struct handle *h = (struct handle *)luaL_checkudata(L, 1, PLAYBACK_MT);
    struct player *pl = h->p;

    if (pl == NULL) {
        return 0;
    }

    __atomic_store_n(&pl->quit, 1, __ATOMIC_RELEASE);
    (void)kosmos_thread_wait((unsigned long)pl->thread);
    h->p = NULL;
    player_free(pl);
    return 0;
}

void kosmos_playback_kit(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "seek",         l_seek },
        { "play",         l_play },
        { "pause",        l_pause },
        { "room",         l_room },
        { "frame",        l_frame },
        { "frame_string", l_frame_string },
        { "ended",        l_ended },
        { "state",        l_state },
        { "close",        l_close },
        { NULL, NULL }
    };

    static const luaL_Reg api[] = {
        { "open", l_open },
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
