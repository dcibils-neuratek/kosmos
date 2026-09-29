/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Synth Kit** - `use("/Kosmos/Kits/synth")` - Groove's sound, on a
 * thread of its own (`roadmap.md` 6zh).
 *
 *   local synth = use("/Kosmos/Kits/synth")
 *   synth.song(song, held)             Groove's tables, as `synth_lua.h` reads;
 *                                      `held` the names a hand is on
 *   synth.start(stream.ring, stream.rate, fewest)
 *                                      `fewest` the periods it may come down
 *                                      to keeping queued, 2 unless said (4i)
 *   synth.play() synth.stop() synth.mode(true)  each answers its number
 *   synth.launch_clip(track, scene) synth.launch_scene(scene)
 *   synth.stop_clip(track) synth.note_on(track, pitch, vel, key)
 *                                      `key` the counter when its key went
 *                                      down, for the way to the ear (4i)
 *   synth.note_off(track, pitch) synth.bend(track, semitones)
 *   synth.release(track)               its held notes let go
 *   synth.listen(page)                 a `/Devices/midi` page this thread
 *                                      takes notes from itself (4i d)
 *   synth.live(track, drum, drums, device, cable, session)
 *                                      where those notes go
 *   synth.hold("t3.p.cut", true)       a person's hand on an automated knob
 *   synth.state(t)                     what is heard, into `t`
 *   synth.export(song, at, capacity)   the song as a WAV, into a region
 *   synth.close()
 *
 * **The engine runs on its own thread**, which is what Diego chose ("All in
 * C, on its own thread"): it renders a period at a time into the audio ring
 * `audio.open` made, straight into the ring's slots, sleeping a scheduler
 * tick when it is far enough ahead. The window - its Lua, its garbage
 * collector, its drawing - is never between a step and its sound (4i).
 *
 * **Far enough ahead is as few periods as this machine holds** (`roadmap.md`
 * 4i, step b). A note is heard after everything queued before it, so a full
 * ring - eight periods, 46 ms - was 46 ms on every key.
 *
 * **It starts full and comes down; it does not start shallow and climb.**
 * It keeps the whole ring, and each second in which every wake found at
 * least two periods still queued it keeps one fewer, to `fewest`. A wake
 * that finds the ring empty - the audio server had nothing of this
 * stream's - keeps one more at once, and that depth becomes the floor it
 * never comes below again. So the depth is found by playing, per machine,
 * and found *from the safe side*: the first version kept two and climbed
 * each time the ring ran dry, which found the same depth by running dry on
 * the way - three times in every start under QEMU - and a ring that has
 * run dry is one late wake from a click (`testing.md` 18.262).
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
#include "midiproto.h"
#include "synth_lua.h"

/* ------------------------------------------------------------ commands */

enum cmd_kind {
    CMD_PLAY, CMD_STOP, CMD_MODE, CMD_LAUNCH_CLIP, CMD_LAUNCH_SCENE,
    CMD_STOP_CLIP, CMD_NOTE_ON, CMD_NOTE_OFF, CMD_BEND, CMD_HOLD, CMD_RELEASE,
    CMD_LIVE,
};

struct cmd {
    enum cmd_kind kind;
    int    a, b;
    double v;
    uint64_t key;                       /* a note's: its key went down */
    uint64_t posted;                    /* ...and the window posted it */
};

#define CMDS 256u

static struct cmd cmds[CMDS];
static uint32_t cmd_head;               /* written by the window's thread */
static uint32_t cmd_tail;               /* written by the audio thread */

/* ------------------------------------------------------------ what is heard */

/*
 * **A note's way to the ear** (`roadmap.md` 4i): when its key went down -
 * the MIDI driver's counter, or the window manager's for a key on the
 * computer - when the window posted it, when this thread took it, and what
 * was already queued ahead of it, in frames: the ring's periods the audio
 * server has not mixed, and the device's it has not played. The note's
 * first sample is in the next period written, so it is heard after those.
 * The last note's, and how many there have been.
 */
struct note_path {
    uint64_t key, posted, applied;
    uint32_t ring, device;
    uint32_t ahead;                     /* periods the kit kept, then */
    uint32_t count;
    bool     from_page;                 /* taken by this thread, not posted */
};

struct heard {
    bool   playing, finished, song_mode, known;
    uint32_t applied;                   /* commands taken before this */
    unsigned long busy;                 /* counter ticks spent rendering */
    long   rendered;                    /* frames rendered, all told */
    struct synth_heard at;
    int    playing_clip[SYNTH_TRACKS], queued[SYNTH_TRACKS];
    long   clip_start[SYNTH_TRACKS];
    double peak[SYNTH_TRACKS];
    unsigned hits[SYNTH_TRACKS][SYNTH_ROWS];
    double peak_l, peak_r;
    struct note_path note;
    uint32_t ahead, dry;                /* periods kept queued; runs dry */
    bool     audio_band;                /* the thread is in the audio band */
    unsigned long worst_pass;           /* counter ticks, between passes */
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
static struct note_path note_path;      /* the audio thread's alone */
/* The audio thread's alone, set by `start` before there is one. */
static uint32_t ahead_kept = 2;         /* periods kept queued */
static uint32_t ahead_floor = 2;        /* never fewer: `fewest`, or a dry */
static uint32_t dry;                    /* times the ring was found empty */
static bool     in_audio_band;          /* the thread got the band it asked */
static unsigned long worst_pass;        /* counter ticks between two passes */
static unsigned long last_pass_at;      /* the counter at the latest pass */
static unsigned long busy;              /* the audio thread's, rendering */

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

/*
 * Where a note stands as it is taken: the frames written and not yet heard,
 * split at the server's `read` - what it has not mixed is the ring's, the
 * rest the device's. With no thread there is no ring and nothing queued.
 */
static void noted_at(uint64_t key, uint64_t posted, bool from_page)
{
    note_path.key = key != 0 ? key : posted;
    note_path.posted = posted;
    note_path.from_page = from_page;
    note_path.applied = kosmos_ticks();
    note_path.ring = note_path.device = 0;

    if (ring != NULL) {
        uint32_t period = ring->period_bytes / 4u;
        uint64_t waiting = audio_ring_delay(ring, period);
        uint64_t unmixed = (uint64_t)(ring->write - ring->read) * period;

        if (unmixed > waiting) unmixed = waiting;

        note_path.ring = (uint32_t)unmixed;
        note_path.device = (uint32_t)(waiting - unmixed);
    }

    note_path.ahead = ahead_kept;

    note_path.count++;
}

static void noted(const struct cmd *c)
{
    noted_at(c->key, c->posted, false);
}

/* ------------------------------------------------------------ live MIDI */
/*
 * **A key reaches the sound without the window** (`roadmap.md` 4i, step d).
 * The window takes MIDI in its pass, and its pass is a frame - under load, a
 * turn among the display band's; a note went through it and waited for it.
 * So the window hands this thread a `/Devices/midi` page of its own
 * (`synth.listen`) and the thread takes new events every pass, a tick at
 * most, and starts the notes itself - in the audio band, above everything
 * that draws. The window keeps a page of its own for what a person sees,
 * the Launchkey's lights and recording, and no longer starts the sound.
 *
 * What is on the clock is done here and nothing else: notes, the sustain
 * pedal and bend. Where a note goes is PulseMusic's rule (`groove/app.lua`,
 * `midiEvent`), from a map the window sends when it changes (`CMD_LIVE`):
 * a key plays the selected track, a drum track's row by its note; a pad on
 * channel 10 plays the drum track's row; and the Launchkey's session pads,
 * on its surface port, are the window's. Each key remembers where its note
 * went, so a release after the selection moved still finds it.
 *
 * The page is read by this thread alone and its `read` is written back, the
 * program's place the driver watches (`midiproto.h`).
 */
struct live_map {
    int      track;                     /* the keys' track, 0-based; -1 none */
    bool     drum;                      /* the keys' track is drums */
    int      drums;                     /* the pads' track, 0-based; -1 none */
    uint32_t surface_device;            /* the Launchkey's surface port, */
    uint8_t  surface_cable;             /* whose session pads are not notes */
    bool     surface_session;
};

static struct midi_ring *live_offered;  /* `listen`'s, taken by the thread */
static struct midi_ring *live_ring;     /* the thread's alone, from here */
static uint32_t live_read;
static struct live_map live = { -1, false, -1, 0, 0, false };
static int8_t live_track[16][128], live_pitch[16][128];   /* a key's note */
static bool live_sustain, live_sustained[16][128];

/* Launchkey Mini MK3's drum pads to Groove's rows, PulseMusic's `PAD_ROW`. */
static const int8_t pad_row[128] = {
    [36] = 1, [37] = 2, [38] = 3, [39] = 4, [44] = 5, [45] = 6, [46] = 7, [47] = 8,
    [40] = 1, [41] = 2, [42] = 3, [43] = 4, [48] = 5, [49] = 6, [50] = 7, [51] = 8,
};

static void live_off(int ch, int key)
{
    if (live_track[ch][key] >= 0) {
        synth_engine_note_off(engine, live_track[ch][key], live_pitch[ch][key]);
    }

    live_track[ch][key] = -1;
    live_sustained[ch][key] = false;
}

static void live_event(const struct midi_ring_event *ev)
{
    unsigned status, kind, ch, d1, d2;

    if ((ev->flags & MIDI_EVENT_SYSEX) != 0 || ev->length == 0) return;

    status = ev->bytes[0];
    kind = status >> 4;
    ch = status & 15u;
    d1 = ev->length > 1 ? (ev->bytes[1] & 127u) : 0;
    d2 = ev->length > 2 ? (ev->bytes[2] & 127u) : 0;

    if (kind == 0x9 && d2 == 0) kind = 0x8;

    if (kind == 0x9 || kind == 0x8) {
        /* The surface's session pads are the window's, as `LK.handle` has it. */
        if (live.surface_session && ev->device == live.surface_device
            && ev->cable == live.surface_cable && ch == 0
            && ((d1 >= 96 && d1 < 104) || (d1 >= 112 && d1 < 120))) {
            return;
        }

        if (kind == 0x8) {
            if (live_sustain && live_track[ch][d1] >= 0) {
                live_sustained[ch][d1] = true;
            } else {
                live_off((int)ch, (int)d1);
            }

            return;
        }

        if (live_track[ch][d1] >= 0 && !live_sustained[ch][d1]) return;   /* held */

        if (live_sustained[ch][d1]) live_off((int)ch, (int)d1);

        int track, pitch;

        if (ch == 9 && pad_row[d1] != 0) {
            track = live.drums;
            pitch = pad_row[d1];
        } else if (live.drum) {
            track = live.track;
            pitch = (int)(d1 % 12u % 8u) + 1;
        } else {
            track = live.track;
            pitch = (int)d1;
        }

        if (track < 0 || track >= SYNTH_TRACKS) return;

        synth_engine_note_on(engine, track, pitch, 0.25 + 0.75 * (double)d2 / 127.0);
        live_track[ch][d1] = (int8_t)track;
        live_pitch[ch][d1] = (int8_t)pitch;
        noted_at(ev->counter, kosmos_ticks(), true);
    } else if (kind == 0xB && d1 == 64) {
        live_sustain = d2 >= 64;

        if (!live_sustain) {
            for (unsigned c = 0; c < 16; c++) {
                for (unsigned k = 0; k < 128; k++) {
                    if (live_sustained[c][k]) live_off((int)c, (int)k);
                }
            }
        }
    } else if (kind == 0xE && live.track >= 0) {
        synth_engine_bend(engine, live.track,
                          ((double)(d1 | (d2 << 7)) - 8192.0) / 8192.0 * 2.0);
    }
}

/* Every event since the last pass, and the page's place written back. */
static void live_take(void)
{
    struct midi_ring *offered = __atomic_exchange_n(&live_offered, NULL, __ATOMIC_ACQ_REL);

    if (offered != NULL) {
        /* From the page's own place, not from now: what arrived between
         * the page being opened and its being handed here is played. */
        live_ring = offered;
        live_read = __atomic_load_n(&offered->read, __ATOMIC_ACQUIRE);

        for (unsigned c = 0; c < 16; c++) {
            for (unsigned k = 0; k < 128; k++) live_track[c][k] = -1;
        }
    }

    if (live_ring == NULL || engine->song == NULL) return;

    uint32_t write = __atomic_load_n(&live_ring->write, __ATOMIC_ACQUIRE);

    if (write - live_read > MIDI_RING_SLOTS) live_read = write - MIDI_RING_SLOTS;

    while (live_read != write) {
        live_event(&live_ring->events[live_read % MIDI_RING_SLOTS]);
        live_read++;
    }

    __atomic_store_n(&live_ring->read, live_read, __ATOMIC_RELEASE);
}

static void apply(const struct cmd *c)
{
    struct synth_engine *e = engine;

    if (e->song == NULL && c->kind != CMD_MODE && c->kind != CMD_LIVE) return;

    switch (c->kind) {
    case CMD_PLAY:         synth_engine_play(e); break;
    case CMD_STOP:         synth_engine_stop(e); break;
    case CMD_MODE:         synth_engine_mode(e, c->a != 0); break;
    case CMD_LAUNCH_CLIP:  synth_engine_launch_clip(e, c->a, c->b); break;
    case CMD_LAUNCH_SCENE: synth_engine_launch_scene(e, c->a); break;
    case CMD_STOP_CLIP:    synth_engine_stop_clip(e, c->a); break;
    case CMD_NOTE_ON:      synth_engine_note_on(e, c->a, c->b, c->v); noted(c); break;
    case CMD_NOTE_OFF:     synth_engine_note_off(e, c->a, c->b); break;
    case CMD_BEND:         synth_engine_bend(e, c->a, c->v); break;
    case CMD_HOLD:         synth_engine_hold(e, c->a, c->b != 0); break;
    case CMD_RELEASE:      synth_engine_release(e, c->a); break;
    case CMD_LIVE:
        /* Packed: a the keys' track, b the drum track, v 1 for a drum
         * track; key the surface's device, cable and session bit. */
        live.track = c->a;
        live.drums = c->b;
        live.drum = c->v != 0;
        live.surface_device = (uint32_t)(c->key >> 16);
        live.surface_cable = (uint8_t)((c->key >> 8) & 255u);
        live.surface_session = (c->key & 1u) != 0;
        break;
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

    heard.applied = cmd_tail;
    heard.busy = busy;
    heard.rendered = e->total;
    heard.playing = e->playing;
    heard.finished = e->finished;
    heard.song_mode = e->song_mode;

    uint32_t period = ring ? ring->period_bytes / 4u : 0;
    long latency = ring ? (long)audio_ring_delay(ring, period) : 0;

    heard.known = synth_engine_heard(e, latency, &heard.at);

    for (int t = 0; t < SYNTH_TRACKS; t++) {
        heard.playing_clip[t] = e->rt[t].playing;
        heard.queued[t] = e->rt[t].queued;
        heard.clip_start[t] = e->rt[t].clip_start;
        heard.peak[t] = e->rt[t].peak;
        memcpy(heard.hits[t], e->rt[t].hits, sizeof heard.hits[t]);

        /* A meter falls, and the engine only ever raises it. */
        e->rt[t].peak *= 0.8;
    }

    heard.peak_l = e->peak_l;
    heard.peak_r = e->peak_r;
    heard.note = note_path;
    heard.ahead = ahead_kept;
    heard.dry = dry;
    heard.audio_band = in_audio_band;
    heard.worst_pass = worst_pass;
    e->peak_l *= 0.8;
    e->peak_r *= 0.8;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&heard_seq, seq + 2, __ATOMIC_RELEASE);
}

static void audio_main(unsigned long arg)
{
    (void)arg;

    /*
     * **Above every window** (`roadmap.md` 4i, step c): this thread is born
     * in the audio band when its program declared `kosmos: needs audio`
     * (`start` below). It used to ask for the band here, in its first line
     * - and a thread at NORMAL with a display-band spinner on every core
     * never reached its first line. `in_audio_band` is set by `start`.
     */

    uint32_t period = ring->period_bytes / 4u;
    bool started = false;

    /* The least queued at any wake in this second, and the wakes in it. */
    uint32_t lowest = UINT32_MAX;
    struct sysinfo info;

    memset(&info, 0, sizeof info);
    (void)kosmos_sysinfo(&info);

    unsigned long second = info.counter_hz ? info.counter_hz : 62500000UL;
    unsigned long second_began = kosmos_ticks();

    unsigned long last_pass = 0;

    while (!__atomic_load_n(&quit, __ATOMIC_ACQUIRE)) {
        /* The longest this thread has been away between two passes: a
         * sleep of one tick, and whatever held it off after that. */
        unsigned long now = kosmos_ticks();

        if (started && last_pass != 0 && now - last_pass > worst_pass) {
            worst_pass = now - last_pass;
        }

        last_pass = now;

        /* Every pass, read by `state` straight: the snapshot is published
         * only when periods are written, and a thread waiting on a full
         * ring passes without writing. */
        __atomic_store_n(&last_pass_at, now, __ATOMIC_RELAXED);

        take_song();
        drain();
        live_take();

        if (started) {
            uint32_t queued = ring->write - ring->read;

            if (queued == 0) {
                /* Run dry: this depth does not hold here. One more, and
                 * never this few again. */
                dry++;

                if (ahead_kept < ring->periods) ahead_kept++;
                if (ahead_floor < ahead_kept) ahead_floor = ahead_kept;

                lowest = UINT32_MAX;
                second_began = kosmos_ticks();
            } else {
                if (queued < lowest) lowest = queued;

                /* A second with two to spare at every wake: one fewer. */
                if (kosmos_ticks() - second_began >= second) {
                    if (lowest >= 2 && ahead_kept > ahead_floor) ahead_kept--;

                    lowest = UINT32_MAX;
                    second_began = kosmos_ticks();
                }
            }
        }

        bool wrote = false;

        while (ring->write - ring->read < ahead_kept && audio_ring_space(ring) > 0) {
            int16_t *slot = (int16_t *)audio_ring_slot(ring, ring->write);
            unsigned long t0 = kosmos_ticks();

            synth_engine_render(engine, lbuf, rbuf, (int)period);

            for (uint32_t i = 0; i < period; i++) {
                slot[i * 2] = (int16_t)(lbuf[i] * 32767.0);
                slot[i * 2 + 1] = (int16_t)(rbuf[i] * 32767.0);
            }

            busy += kosmos_ticks() - t0;

            audio_ring_publish(ring, ring->write + 1);
            wrote = true;
            started = true;

            /* A note asked for between two periods is played in the next. */
            drain();
            live_take();
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

/*
 * A command, for the thread - or done at once when there is none. Answers
 * its number: `state().applied` reaching it says the state read includes
 * it, which is how the window tells "play was pressed and not yet heard"
 * from "the song has ended by itself".
 */
static int post_at(lua_State *L, enum cmd_kind kind, int a, int b, double v,
                   uint64_t key)
{
    struct cmd c = { kind, a, b, v, key, kosmos_ticks() };

    if (!ensure_engine(L)) return 0;

    uint32_t head = cmd_head;

    if (!running()) {
        apply(&c);
        cmd_head = cmd_tail = head + 1;
        lua_pushinteger(L, head + 1);
        return 1;
    }

    if (head - __atomic_load_n(&cmd_tail, __ATOMIC_ACQUIRE) >= CMDS) {
        return luaL_error(L, "the Synth Kit's engine is not keeping up");
    }

    cmds[head % CMDS] = c;
    __atomic_store_n(&cmd_head, head + 1, __ATOMIC_RELEASE);
    lua_pushinteger(L, head + 1);
    return 1;
}

static int post(lua_State *L, enum cmd_kind kind, int a, int b, double v)
{
    return post_at(L, kind, a, b, v, 0);
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

    synth_song_holds_from_lua(L, 2, song);
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

/*
 * synth.listen(page) - a `/Devices/midi` page (`midi.open()`'s `at`) this
 * thread takes notes from itself (4i d). True once the thread has it to take;
 * false when there is no thread, and the window plays its notes as before.
 */
static int l_listen(lua_State *L)
{
    struct midi_ring *r = (struct midi_ring *)(uintptr_t)luaL_checkinteger(L, 1);

    luaL_argcheck(L, r != NULL && r->slots == MIDI_RING_SLOTS, 1,
                  "not a /Devices/midi page");

    if (!running()) {
        lua_pushboolean(L, 0);
        return 1;
    }

    __atomic_store_n(&live_offered, r, __ATOMIC_RELEASE);
    lua_pushboolean(L, 1);
    return 1;
}

/*
 * synth.live(track, drum, drums, device, cable, session) - where the page's
 * notes go: the keys' track and whether it is drums, the pads' drum track
 * (tracks 1 to 8, 0 for none), and the Launchkey's surface port - its
 * device and cable, and whether its pads are in session mode, when they are
 * the window's rather than notes.
 */
static int l_live(lua_State *L)
{
    lua_Integer track = luaL_checkinteger(L, 1);
    int drum = lua_toboolean(L, 2);
    lua_Integer drums = luaL_checkinteger(L, 3);
    lua_Integer device = luaL_optinteger(L, 4, 0);
    lua_Integer cable = luaL_optinteger(L, 5, 0);
    int session = lua_toboolean(L, 6);

    luaL_argcheck(L, track >= 0 && track <= SYNTH_TRACKS, 1, "a track is 1 to 8, or 0");
    luaL_argcheck(L, drums >= 0 && drums <= SYNTH_TRACKS, 3, "a track is 1 to 8, or 0");
    luaL_argcheck(L, device >= 0 && device <= 0xFFFFFFFF && cable >= 0 && cable < 16, 4,
                  "a device's id and a cable");

    return post_at(L, CMD_LIVE, (int)track - 1, (int)drums - 1, drum ? 1.0 : 0.0,
                   ((uint64_t)device << 16) | ((uint64_t)cable << 8) | (session ? 1u : 0u));
}

static int l_note_on(lua_State *L)
{
    int t = track_arg(L, 1);
    lua_Integer p = luaL_checkinteger(L, 2);
    double v = luaL_optnumber(L, 3, 0.8);

    lua_Integer key = luaL_optinteger(L, 4, 0);

    luaL_argcheck(L, p >= 0 && p <= 127, 2, "a pitch is 0 to 127");
    return post_at(L, CMD_NOTE_ON, t, (int)p, v < 0 ? 0 : v > 2 ? 2 : v,
                   key > 0 ? (uint64_t)key : 0);
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

static int l_release(lua_State *L)
{
    return post(L, CMD_RELEASE, track_arg(L, 1), 0, 0);
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

    lua_Integer fewest = luaL_optinteger(L, 3, 2);

    luaL_argcheck(L, fewest >= 1 && fewest <= (lua_Integer)r->periods, 3,
                  "keep between one period and the ring's");

    ahead_kept = r->periods;
    ahead_floor = (uint32_t)fewest;
    dry = 0;
    worst_pass = 0;
    ring = r;
    __atomic_store_n(&quit, 0, __ATOMIC_RELEASE);

    /*
     * In the audio band from its first instruction, if this program may put
     * a thread there; at NORMAL otherwise, as before, and it says so in
     * `synth.state().audio_band`.
     */
    long index = kosmos_thread_start_audio(audio_main, 0);

    in_audio_band = index >= 0;

    if (index < 0) {
        index = kosmos_thread_start(audio_main, 0);
    }

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
 *   applied                      the commands it includes, by their number
 *   busy, rendered               counter ticks spent rendering and frames
 *                                rendered, all told: PulseMusic's DSP load
 *   playing, finished, song      booleans
 *   step                         the step heard, with its fraction, or nil
 *   chain, section_step          the section and the step inside it
 *   scene                        the scene launched last, from its bar; 0
 *   peak_l, peak_r               the master's meters
 *   tracks[1..8]                 { playing, queued, start, peak, hits = {8} }
 *                                `start` the step its clip began on
 *   notes                        notes played, all told; and the last one's
 *   note_key, note_posted,       way to the ear: counters when its key went
 *   note_applied                 down, when it was posted and when taken,
 *   note_ring, note_device       and the frames queued ahead of it, the
 *                                ring's and the device's
 *   note_ahead                   the periods the kit kept queued, then
 *   ahead, dry                   the periods it keeps now, and how often
 *                                the ring has run dry
 *   audio_band                   whether its thread is in the audio band
 *   worst_pass                   the longest between two of its passes, in
 *                                counter ticks: a tick's sleep, and what
 *                                held it off after
 *   last_pass                    the counter at its latest pass: a thread
 *                                held off still is away since then, which
 *                                no pair of passes can say
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

    set_integer(L, "applied", copy.applied);
    set_integer(L, "busy", (lua_Integer)copy.busy);
    set_integer(L, "rendered", copy.rendered);
    set_boolean(L, "playing", copy.playing);
    set_boolean(L, "finished", copy.finished);
    set_boolean(L, "song", copy.song_mode);

    if (copy.known) {
        set_number(L, "step", copy.at.step);
        set_integer(L, "chain", copy.at.chain_pos);
        set_integer(L, "section_step", copy.at.section_step);
        set_integer(L, "scene", copy.at.scene);
    } else {
        lua_pushnil(L);
        lua_setfield(L, -2, "step");
        set_integer(L, "scene", 0);
    }

    set_number(L, "peak_l", copy.peak_l);
    set_number(L, "peak_r", copy.peak_r);
    set_integer(L, "notes", copy.note.count);
    set_integer(L, "note_key", (lua_Integer)copy.note.key);
    set_integer(L, "note_posted", (lua_Integer)copy.note.posted);
    set_integer(L, "note_applied", (lua_Integer)copy.note.applied);
    set_integer(L, "note_ring", copy.note.ring);
    set_integer(L, "note_device", copy.note.device);
    set_integer(L, "note_ahead", copy.note.ahead);
    set_boolean(L, "note_from_page", copy.note.from_page);
    set_integer(L, "ahead", copy.ahead);
    set_integer(L, "dry", copy.dry);
    set_boolean(L, "audio_band", copy.audio_band);
    set_integer(L, "worst_pass", (lua_Integer)copy.worst_pass);
    set_integer(L, "last_pass",
                (lua_Integer)__atomic_load_n(&last_pass_at, __ATOMIC_RELAXED));

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
        set_integer(L, "start", copy.clip_start[t]);
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

/*
 * synth.export(song, at, capacity) - the song, from its first section to its
 * last and four seconds after for the echoes to die, as PulseMusic's EXPORT
 * WAV renders it: a WAV of 16-bit stereo at 44.1 kHz, header and all, written
 * into the region at `at`, which has `capacity` bytes. Answers the file's
 * length in bytes and its seconds, and true when the region was too small
 * for the whole of it and the file stops where the room did.
 *
 * **An engine of its own**, made here and gone after, so the one playing
 * goes on playing and nothing crosses to its thread; and seeded the same
 * every time, so a song exported twice is the same file twice. The caller
 * writes the region with `fs.write_from`, which takes a file of any size
 * where `fs.write` takes a string.
 */
struct exporter {
    struct synth_engine engine;
    double l[SYNTH_BLOCK], r[SYNTH_BLOCK];
};

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static int l_export(lua_State *L)
{
    uintptr_t at = (uintptr_t)luaL_checkinteger(L, 2);
    lua_Integer capacity = luaL_checkinteger(L, 3);

    luaL_argcheck(L, at != 0, 2, "not a region");
    luaL_argcheck(L, capacity >= 44 + 4 * SYNTH_BLOCK, 3, "no room for a period");

    struct synth_song *song = synth_song_from_lua(L, 1);

    if (song == NULL) return lua_error(L);

    struct exporter *x = malloc(sizeof *x);

    if (x == NULL) {
        synth_song_free(song);
        return luaL_error(L, "no memory to export with");
    }

    synth_engine_init(&x->engine, 1);
    synth_engine_set_song(&x->engine, song, false);
    synth_engine_mode(&x->engine, true);
    synth_engine_play(&x->engine);

    uint8_t *out = (uint8_t *)at;
    int16_t *pcm = (int16_t *)(out + 44);
    long room = (long)((capacity - 44) / 4);
    long limit = (long)SYNTH_RATE * 60 * 30;       /* half an hour, as PulseMusic */
    long tail = (long)SYNTH_RATE * 4;
    long frames = 0;
    bool cut = false;

    for (;;) {
        if (frames + SYNTH_BLOCK > room) {
            cut = true;
            break;
        }

        if (frames >= limit) break;

        synth_engine_render(&x->engine, x->l, x->r, SYNTH_BLOCK);

        for (int i = 0; i < SYNTH_BLOCK; i++) {
            pcm[(frames + i) * 2] = (int16_t)(x->l[i] * 32767.0);
            pcm[(frames + i) * 2 + 1] = (int16_t)(x->r[i] * 32767.0);
        }

        frames += SYNTH_BLOCK;

        if (x->engine.finished) {
            tail -= SYNTH_BLOCK;
            if (tail <= 0) break;
        }
    }

    synth_song_free(synth_engine_set_song(&x->engine, NULL, false));
    free(x);

    uint32_t bytes = (uint32_t)frames * 4u;

    memcpy(out, "RIFF", 4);
    put32(out + 4, 36u + bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16);
    put16(out + 20, 1);                             /* PCM */
    put16(out + 22, 2);                             /* stereo */
    put32(out + 24, SYNTH_RATE);
    put32(out + 28, SYNTH_RATE * 4);
    put16(out + 32, 4);
    put16(out + 34, 16);
    memcpy(out + 36, "data", 4);
    put32(out + 40, bytes);

    lua_pushinteger(L, 44 + (lua_Integer)bytes);
    lua_pushnumber(L, (double)frames / SYNTH_RATE);
    lua_pushboolean(L, cut);
    return 3;
}

void kosmos_synth_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "song", l_song }, { "start", l_start }, { "close", l_close },
        { "play", l_play }, { "stop", l_stop }, { "mode", l_mode },
        { "launch_clip", l_launch_clip }, { "launch_scene", l_launch_scene },
        { "stop_clip", l_stop_clip }, { "note_on", l_note_on },
        { "listen", l_listen }, { "live", l_live },
        { "note_off", l_note_off }, { "bend", l_bend }, { "hold", l_hold },
        { "release", l_release }, { "state", l_state }, { "export", l_export },
        { NULL, NULL },
    };

    luaL_newlib(L, api);
}
