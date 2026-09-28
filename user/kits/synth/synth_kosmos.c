/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Synth Kit** - `use("/Kosmos/Kits/synth")` - Groove's sound, on a
 * thread of its own (`roadmap.md` 6zh).
 *
 *   local synth = use("/Kosmos/Kits/synth")
 *   synth.song(song)                   Groove's tables, as `synth_lua.h` reads
 *   synth.start(stream.ring, stream.rate)
 *   synth.play() synth.stop() synth.mode(true)
 *   synth.launch_clip(track, scene) synth.launch_scene(scene)
 *   synth.stop_clip(track) synth.note_on(track, pitch, vel)
 *   synth.note_off(track, pitch) synth.bend(track, semitones)
 *   synth.hold("t3.p.cut", true)       a person's hand on an automated knob
 *   synth.state(t)                     what is heard, into `t`
 *   synth.close()
 *
 * **The engine runs on its own thread**, which is what Diego chose ("All in
 * C, on its own thread"): it renders a period at a time into the audio ring
 * `audio.open` made, straight into the ring's slots, sleeping a scheduler
 * tick when the ring is full. The window - its Lua, its garbage collector,
 * its drawing - is never between a step and its sound (4i).
 *
 * **Three things cross between the two threads, and each one way:**
 *
 * - a song, made on the window's thread and handed over whole as a pointer
 *   (`pending`), taken at a period's boundary; the one it replaces comes back
 *   (`retired`) to be freed where it was made - the audio thread never
 *   touches the heap;
 * - commands - play, a note, a clip - in a ring of fixed size, one writer and
 *   one reader, the indices the only thing both touch (`CLAUDE.md`'s rule
 *   for a region two processes share, which holds for two threads too);
 * - what is heard, published after each period under a sequence number, and
 *   read by the window as a copy.
 *
 * Before `start`, and after `close`, there is no thread, and everything is
 * done at once on the caller's: which is how a song is exported.
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
#include "synth_lua.h"

/* ------------------------------------------------------------ commands */

enum cmd_kind {
    CMD_PLAY, CMD_STOP, CMD_MODE, CMD_LAUNCH_CLIP, CMD_LAUNCH_SCENE,
    CMD_STOP_CLIP, CMD_NOTE_ON, CMD_NOTE_OFF, CMD_BEND, CMD_HOLD,
};

struct cmd {
    enum cmd_kind kind;
    int    a, b;
    double v;
};

#define CMDS 256u

static struct cmd cmds[CMDS];
static uint32_t cmd_head;               /* written by the window's thread */
static uint32_t cmd_tail;               /* written by the audio thread */

/* ------------------------------------------------------------ what is heard */

struct heard {
    bool   playing, finished, song_mode, known;
    double step;
    int    chain_pos;
    long   section_step;
    int    playing_clip[SYNTH_TRACKS], queued[SYNTH_TRACKS];
    double peak[SYNTH_TRACKS];
    unsigned hits[SYNTH_TRACKS][SYNTH_ROWS];
    double peak_l, peak_r;
};

static struct heard heard;
static uint32_t heard_seq;              /* odd while it is being written */

/* ------------------------------------------------------------ the engine */

static struct synth_engine *engine;
static struct synth_song *pending;      /* a song for the thread to take */
static struct synth_song *retired;      /* one it gave back, to be freed */
static struct synth_song *latest;       /* the last made, for `hold`'s names */

static struct audio_ring *ring;
static long thread_index = -1;
static int quit;

static double lbuf[SYNTH_BLOCK], rbuf[SYNTH_BLOCK];

static bool ensure_engine(lua_State *L)
{
    if (engine) return true;

    engine = malloc(sizeof *engine);

    if (engine == NULL) {
        luaL_error(L, "no memory for the Synth Kit's engine");
        return false;
    }

    synth_engine_init(engine, kosmos_ticks() | 1u);
    return true;
}

static void apply(const struct cmd *c)
{
    struct synth_engine *e = engine;

    if (e->song == NULL && c->kind != CMD_MODE) return;

    switch (c->kind) {
    case CMD_PLAY:         synth_engine_play(e); break;
    case CMD_STOP:         synth_engine_stop(e); break;
    case CMD_MODE:         synth_engine_mode(e, c->a != 0); break;
    case CMD_LAUNCH_CLIP:  synth_engine_launch_clip(e, c->a, c->b); break;
    case CMD_LAUNCH_SCENE: synth_engine_launch_scene(e, c->a); break;
    case CMD_STOP_CLIP:    synth_engine_stop_clip(e, c->a); break;
    case CMD_NOTE_ON:      synth_engine_note_on(e, c->a, c->b, c->v); break;
    case CMD_NOTE_OFF:     synth_engine_note_off(e, c->a, c->b); break;
    case CMD_BEND:         synth_engine_bend(e, c->a, c->v); break;
    case CMD_HOLD:         synth_engine_hold(e, c->a, c->b != 0); break;
    }
}

/* The audio thread's half: what the window asked for, in order. */
static void drain(void)
{
    uint32_t head = __atomic_load_n(&cmd_head, __ATOMIC_ACQUIRE);
    uint32_t tail = cmd_tail;

    while (tail != head) {
        apply(&cmds[tail % CMDS]);
        tail++;
    }

    __atomic_store_n(&cmd_tail, tail, __ATOMIC_RELEASE);
}

/* A song, if one is waiting and the last one given back has been freed. */
static void take_song(void)
{
    if (__atomic_load_n(&retired, __ATOMIC_ACQUIRE) != NULL) return;

    struct synth_song *song = __atomic_exchange_n(&pending, NULL, __ATOMIC_ACQ_REL);

    if (song != NULL) {
        struct synth_song *old = synth_engine_set_song(engine, song, true);

        __atomic_store_n(&retired, old, __ATOMIC_RELEASE);
    }
}

static void publish(void)
{
    struct synth_engine *e = engine;
    uint32_t seq = heard_seq;

    __atomic_store_n(&heard_seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    heard.playing = e->playing;
    heard.finished = e->finished;
    heard.song_mode = e->song_mode;

    uint32_t period = ring ? ring->period_bytes / 4u : 0;
    long latency = ring ? (long)audio_ring_delay(ring, period) : 0;

    heard.known = synth_engine_heard(e, latency, &heard.step, &heard.chain_pos,
                                     &heard.section_step);

    for (int t = 0; t < SYNTH_TRACKS; t++) {
        heard.playing_clip[t] = e->rt[t].playing;
        heard.queued[t] = e->rt[t].queued;
        heard.peak[t] = e->rt[t].peak;
        memcpy(heard.hits[t], e->rt[t].hits, sizeof heard.hits[t]);

        /* A meter falls, and the engine only ever raises it. */
        e->rt[t].peak *= 0.8;
    }

    heard.peak_l = e->peak_l;
    heard.peak_r = e->peak_r;
    e->peak_l *= 0.8;
    e->peak_r *= 0.8;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&heard_seq, seq + 2, __ATOMIC_RELEASE);
}

static void audio_main(unsigned long arg)
{
    (void)arg;

    uint32_t period = ring->period_bytes / 4u;

    while (!__atomic_load_n(&quit, __ATOMIC_ACQUIRE)) {
        take_song();
        drain();

        bool wrote = false;

        while (audio_ring_space(ring) > 0) {
            int16_t *slot = (int16_t *)audio_ring_slot(ring, ring->write);

            synth_engine_render(engine, lbuf, rbuf, (int)period);

            for (uint32_t i = 0; i < period; i++) {
                slot[i * 2] = (int16_t)(lbuf[i] * 32767.0);
                slot[i * 2 + 1] = (int16_t)(rbuf[i] * 32767.0);
            }

            audio_ring_publish(ring, ring->write + 1);
            wrote = true;

            /* A note asked for between two periods is played in the next. */
            drain();
        }

        if (wrote) publish();

        kosmos_sleep(1);
    }

    kosmos_thread_exit(0);
}

/* ------------------------------------------------------------ from Lua */

static bool running(void)
{
    return thread_index >= 0;
}

/* The retired song, freed here where it was made. */
static void collect(void)
{
    struct synth_song *old = __atomic_exchange_n(&retired, NULL, __ATOMIC_ACQ_REL);

    if (old != NULL && old != latest) synth_song_free(old);
}

static int post(lua_State *L, enum cmd_kind kind, int a, int b, double v)
{
    struct cmd c = { kind, a, b, v };

    if (!ensure_engine(L)) return 0;

    if (!running()) {
        apply(&c);
        return 0;
    }

    uint32_t head = cmd_head;

    if (head - __atomic_load_n(&cmd_tail, __ATOMIC_ACQUIRE) >= CMDS) {
        return luaL_error(L, "the Synth Kit's engine is not keeping up");
    }

    cmds[head % CMDS] = c;
    __atomic_store_n(&cmd_head, head + 1, __ATOMIC_RELEASE);
    return 0;
}

static int track_arg(lua_State *L, int index)
{
    lua_Integer t = luaL_checkinteger(L, index);

    luaL_argcheck(L, t >= 1 && t <= SYNTH_TRACKS, index, "a track is 1 to 8");
    return (int)t - 1;
}

static int scene_arg(lua_State *L, int index)
{
    lua_Integer s = luaL_checkinteger(L, index);

    luaL_argcheck(L, s >= 0 && s <= SYNTH_SCENES, index, "a scene is 1 to 8, or 0");
    return (int)s;
}

static int l_song(lua_State *L)
{
    if (!ensure_engine(L)) return 0;

    struct synth_song *song = synth_song_from_lua(L, 1);

    if (song == NULL) return lua_error(L);

    collect();

    if (!running()) {
        synth_song_free(synth_engine_set_song(engine, song, true));
        latest = song;
        return 0;
    }

    struct synth_song *unused = __atomic_exchange_n(&pending, song, __ATOMIC_ACQ_REL);

    /* Never taken: the thread had not got to it before this one came. */
    if (unused != NULL) synth_song_free(unused);

    latest = song;
    return 0;
}

static int l_play(lua_State *L)  { return post(L, CMD_PLAY, 0, 0, 0); }
static int l_stop(lua_State *L)  { return post(L, CMD_STOP, 0, 0, 0); }

static int l_mode(lua_State *L)
{
    return post(L, CMD_MODE, lua_toboolean(L, 1), 0, 0);
}

static int l_launch_clip(lua_State *L)
{
    return post(L, CMD_LAUNCH_CLIP, track_arg(L, 1), scene_arg(L, 2), 0);
}

static int l_launch_scene(lua_State *L)
{
    int s = scene_arg(L, 1);

    luaL_argcheck(L, s >= 1, 1, "a scene is 1 to 8");
    return post(L, CMD_LAUNCH_SCENE, s, 0, 0);
}

static int l_stop_clip(lua_State *L)
{
    return post(L, CMD_STOP_CLIP, track_arg(L, 1), 0, 0);
}

static int l_note_on(lua_State *L)
{
    int t = track_arg(L, 1);
    lua_Integer p = luaL_checkinteger(L, 2);
    double v = luaL_optnumber(L, 3, 0.8);

    luaL_argcheck(L, p >= 0 && p <= 127, 2, "a pitch is 0 to 127");
    return post(L, CMD_NOTE_ON, t, (int)p, v < 0 ? 0 : v > 2 ? 2 : v);
}

static int l_note_off(lua_State *L)
{
    int t = track_arg(L, 1);
    lua_Integer p = luaL_checkinteger(L, 2);

    luaL_argcheck(L, p >= 0 && p <= 127, 2, "a pitch is 0 to 127");
    return post(L, CMD_NOTE_OFF, t, (int)p, 0);
}

static int l_bend(lua_State *L)
{
    double semis = luaL_checknumber(L, 2);

    return post(L, CMD_BEND, track_arg(L, 1), 0, semis < -24 ? -24 : semis > 24 ? 24 : semis);
}

static int l_hold(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int target = latest ? synth_song_target(latest, name) : -1;

    if (target < 0) return 0;          /* not automated: nothing to hold */

    return post(L, CMD_HOLD, target, lua_toboolean(L, 2), 0);
}

/*
 * synth.start(ring, rate) - the thread, rendering into the ring `audio.open`
 * made. The engine is PulseMusic's, at 44.1 kHz and nothing else.
 */
static int l_start(lua_State *L)
{
    struct audio_ring *r = (struct audio_ring *)(uintptr_t)luaL_checkinteger(L, 1);
    lua_Integer rate = luaL_checkinteger(L, 2);

    if (!ensure_engine(L)) return 0;
    if (running()) return luaL_error(L, "the Synth Kit's engine is already running");

    luaL_argcheck(L, audio_ring_valid(r) && r->period_bytes % 4u == 0
                     && r->period_bytes / 4u <= SYNTH_BLOCK, 1,
                  "not a ring of 16-bit stereo periods it can fill");
    luaL_argcheck(L, rate == SYNTH_RATE, 2, "the Synth Kit plays at 44100 a second");

    ring = r;
    __atomic_store_n(&quit, 0, __ATOMIC_RELEASE);

    long index = kosmos_thread_start(audio_main, 0);

    if (index < 0) {
        ring = NULL;
        return luaL_error(L, "no thread for the Synth Kit's engine (%d)", (int)index);
    }

    thread_index = index;
    return 0;
}

static int l_close(lua_State *L)
{
    (void)L;

    if (running()) {
        __atomic_store_n(&quit, 1, __ATOMIC_RELEASE);
        (void)kosmos_thread_wait((unsigned long)thread_index);
        thread_index = -1;
        ring = NULL;
        drain();
    }

    collect();

    struct synth_song *left = __atomic_exchange_n(&pending, NULL, __ATOMIC_ACQ_REL);

    if (left != NULL) {
        synth_song_free(synth_engine_set_song(engine, left, true));
    }

    return 0;
}

static void set_number(lua_State *L, const char *key, double v)
{
    lua_pushnumber(L, v);
    lua_setfield(L, -2, key);
}

static void set_integer(lua_State *L, const char *key, lua_Integer v)
{
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}

static void set_boolean(lua_State *L, const char *key, bool v)
{
    lua_pushboolean(L, v);
    lua_setfield(L, -2, key);
}

/*
 * synth.state(t) - what is heard, into `t` (made when not given), reused
 * frame after frame so a window drawing it makes no garbage:
 *
 *   playing, finished, song      booleans
 *   step                         the step heard, with its fraction, or nil
 *   chain, section_step          the section and the step inside it
 *   peak_l, peak_r               the master's meters
 *   tracks[1..8]                 { playing, queued, peak, hits = {8} }
 */
static int l_state(lua_State *L)
{
    struct heard copy;

    if (!ensure_engine(L)) return 0;

    collect();

    if (running()) {
        uint32_t s1, s2;

        do {
            s1 = __atomic_load_n(&heard_seq, __ATOMIC_ACQUIRE);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            copy = heard;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            s2 = __atomic_load_n(&heard_seq, __ATOMIC_ACQUIRE);
        } while (s1 != s2 || (s1 & 1u));
    } else {
        ring = NULL;
        publish();
        copy = heard;
    }

    if (lua_type(L, 1) == LUA_TTABLE) {
        lua_settop(L, 1);
    } else {
        lua_settop(L, 0);
        lua_createtable(L, 0, 10);
    }

    set_boolean(L, "playing", copy.playing);
    set_boolean(L, "finished", copy.finished);
    set_boolean(L, "song", copy.song_mode);

    if (copy.known) {
        set_number(L, "step", copy.step);
        set_integer(L, "chain", copy.chain_pos);
        set_integer(L, "section_step", copy.section_step);
    } else {
        lua_pushnil(L);
        lua_setfield(L, -2, "step");
    }

    set_number(L, "peak_l", copy.peak_l);
    set_number(L, "peak_r", copy.peak_r);

    if (lua_getfield(L, -1, "tracks") != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_createtable(L, SYNTH_TRACKS, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "tracks");
    }

    for (int t = 0; t < SYNTH_TRACKS; t++) {
        if (lua_geti(L, -1, t + 1) != LUA_TTABLE) {
            lua_pop(L, 1);
            lua_createtable(L, 0, 4);
            lua_pushvalue(L, -1);
            lua_seti(L, -3, t + 1);
        }

        set_integer(L, "playing", copy.playing_clip[t]);
        set_integer(L, "queued", copy.queued[t]);
        set_number(L, "peak", copy.peak[t]);

        if (lua_getfield(L, -1, "hits") != LUA_TTABLE) {
            lua_pop(L, 1);
            lua_createtable(L, SYNTH_ROWS, 0);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, "hits");
        }

        for (int r = 0; r < SYNTH_ROWS; r++) {
            lua_pushinteger(L, copy.hits[t][r]);
            lua_seti(L, -2, r + 1);
        }

        lua_pop(L, 2);
    }

    lua_pop(L, 1);
    return 1;
}

void kosmos_synth_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "song", l_song }, { "start", l_start }, { "close", l_close },
        { "play", l_play }, { "stop", l_stop }, { "mode", l_mode },
        { "launch_clip", l_launch_clip }, { "launch_scene", l_launch_scene },
        { "stop_clip", l_stop_clip }, { "note_on", l_note_on },
        { "note_off", l_note_off }, { "bend", l_bend }, { "hold", l_hold },
        { "state", l_state }, { NULL, NULL },
    };

    luaL_newlib(L, api);
}
