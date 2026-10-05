/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Synth Kit's engine: a song, its sequencer and its mixer (`roadmap.md`
 * 6zh, Groove).
 *
 * PulseMusic's `engine.lua` in C: eight tracks - drums or a synthesiser -
 * eight scenes of clips, a chain of sections for a song with automation
 * lanes in them, a sequencer accurate to the sample with swing, a kick that
 * ducks what asks to be ducked, two sends into a delay and a reverb, and a
 * master that clips softly. The song is a struct the kit builds from the
 * Lua tables Groove edits (`synth_kosmos.c`), and the engine plays whichever
 * song it was last handed.
 *
 * Pure, as `synth_dsp.c` is: `tools/test_synth.c` drives it on the Mac.
 */
#ifndef KOSMOS_SYNTH_ENGINE_H
#define KOSMOS_SYNTH_ENGINE_H

#include "synth_dsp.h"

#define SYNTH_TRACKS    8
#define SYNTH_SCENES    8
#define SYNTH_ROWS      8
#define SYNTH_STEPS     64
#define SYNTH_OFFS      64          /* note-offs a track can be waiting on */
#define SYNTH_DRUMVOICES 24
#define SYNTH_TICKS     48          /* the ticks remembered for what is heard */

/* The master effects, in `presets.lua`'s order. */
enum {
    FX_DTIME, FX_DFB, FX_DDAMP, FX_RSIZE, FX_RDAMP, FX_DUCKREL, FX_DRET, FX_RRET,
    SYNTH_FX
};

extern const char *const synth_fx_names[SYNTH_FX];

struct synth_note {
    int    step, pitch, len;
    double vel;
};

struct synth_clip {
    bool   present;
    int    len;                                 /* 16, 32 or 64 steps */
    float  steps[SYNTH_ROWS][SYNTH_STEPS];      /* drums: velocity, 0 = none */
    int    notes_count;
    struct synth_note *notes;                   /* synths */
};

struct synth_track {
    bool   drum;
    double vol, pan, send_a, send_b, duck;
    bool   mute, solo;
    int    kit;                                 /* 0 to SYNTH_KITS - 1 */
    struct synth_params params;
    struct synth_row rows[SYNTH_ROWS];
    struct synth_clip clips[SYNTH_SCENES];
};

/* What an automation lane drives, and how a 0-to-1 value becomes its own. */
enum synth_target_kind { TARGET_PARAM, TARGET_ROW, TARGET_MIX, TARGET_FX };

struct synth_target {
    char   name[24];                            /* as the song names it */
    enum synth_target_kind kind;
    int    track, row, index;
    double min, max;
    bool   exp, stepped;
    bool   has_base;
    double base;                                /* the song's `autoBase` */
};

struct synth_lane {
    int    target;                              /* into the song's targets */
    int    count;                               /* steps: bars times 16 */
    double *values;                             /* below 0 where nothing is set */
};

struct synth_section {
    int    scene;                               /* 1 to SYNTH_SCENES */
    int    bars;
    int    lanes_count;
    struct synth_lane *lanes;
};

struct synth_ramp {
    bool   on;
    double from, to, pos, len;
};

struct synth_song {
    double bpm, swing, master;
    double fx[SYNTH_FX];
    struct synth_track tracks[SYNTH_TRACKS];
    int    sections_count;
    struct synth_section *sections;
    int    targets_count;
    struct synth_target *targets;

    /*
     * A glide and a hold for each target, which the engine keeps while it
     * plays - here rather than in the engine because the song is made on
     * the window's thread and handed over whole, so the audio thread never
     * allocates.
     */
    struct synth_ramp *ramps;
    bool   *held;
};

/* The mix fields a lane can drive: `vol`, `pan`, `sendA`, `sendB`, `duck`. */
enum { MIX_VOL, MIX_PAN, MIX_SENDA, MIX_SENDB, MIX_DUCK, SYNTH_MIX };

/*
 * A target named as PulseMusic names it - `t3.p.cut`, `t1.r2.decay`,
 * `t3.m.sendA`, `fx.dFb` - resolved; false when it names nothing.
 */
bool synth_target_parse(const char *name, struct synth_target *out);

/* A value between 0 and 1 as the target's own. */
double synth_from_norm(const struct synth_target *t, double n);

/* Where in the song a target's value lives, or NULL. */
double *synth_target_value(struct synth_song *song, const struct synth_target *t);

void synth_song_free(struct synth_song *song);

/* ------------------------------------------------------------ the engine */

struct synth_offs {
    int  pitch;
    long at;
};

struct synth_track_rt {
    struct synth_voice voices[8];
    struct synth_drum_voice dv[SYNTH_DRUMVOICES];
    int    dv_count;
    int    playing;                             /* scene 1 to 8, or 0 */
    int    queued;                              /* -1 none, 0 stop, 1 to 8 */
    long   clip_start;
    double peak, lfo_ph, bend, bend_s;
    struct synth_offs offs[SYNTH_OFFS];
    int    offs_count;
    unsigned hits[SYNTH_ROWS];
};

struct synth_tick {
    long   total, step;
    double dur;
    int    chain_pos;
    long   section_step;
    int    scene;
};

struct synth_engine {
    struct synth_song *song;
    struct synth_track_rt rt[SYNTH_TRACKS];
    struct synth_random rnd;
    uint64_t age;

    bool   song_mode, playing, finished;
    long   step, total;
    double until_tick;
    double duck_env, duck;
    double peak_l, peak_r;
    int    chain_pos, chain_left, pending_scene;

    /*
     * The scene launched last, from the bar it took effect - which is what
     * Groove's performance recording writes into the song, a bar at a time,
     * as PulseMusic's `curScene`. 0 until a scene is launched.
     */
    int    scene;
    long   section_start;

    struct synth_tick ticks[SYNTH_TICKS];
    int    ticks_count;

    double tbuf[SYNTH_BLOCK], tmp[SYNTH_BLOCK];
    double send_a[SYNTH_BLOCK], send_b[SYNTH_BLOCK], duck_buf[SYNTH_BLOCK];
    struct synth_delay delay;
    struct synth_reverb reverb;
};

void synth_engine_init(struct synth_engine *e, uint64_t seed);

/*
 * Hands the engine a song, and answers with the one it had, for the caller
 * to free. `keep` keeps what is playing - the voices, the clips, the step -
 * which is how an edit reaches a song while it plays; without it the engine
 * starts afresh, stopped.
 */
struct synth_song *synth_engine_set_song(struct synth_engine *e,
                                         struct synth_song *song, bool keep);

void synth_engine_play(struct synth_engine *e);
void synth_engine_stop(struct synth_engine *e);
void synth_engine_mode(struct synth_engine *e, bool song_mode);
void synth_engine_launch_clip(struct synth_engine *e, int track, int scene);
void synth_engine_launch_scene(struct synth_engine *e, int scene);
void synth_engine_stop_clip(struct synth_engine *e, int track);
void synth_engine_note_on(struct synth_engine *e, int track, int pitch, double vel);
void synth_engine_note_off(struct synth_engine *e, int track, int pitch);
void synth_engine_bend(struct synth_engine *e, int track, double semitones);

/* Every held note of a track let go, as a clip's change of scene does. */
void synth_engine_release(struct synth_engine *e, int track);
void synth_engine_hold(struct synth_engine *e, int target, bool held);

/* `n` frames of stereo into `l` and `r`, in -1 to 1. */
void synth_engine_render(struct synth_engine *e, double *l, double *r, int n);

/*
 * What is being heard, `latency` frames behind what has been rendered: the
 * step with its fraction, the section and the step inside it, and the scene
 * launched last. False before the first tick.
 */
struct synth_heard {
    double step;
    int    chain_pos;
    long   section_step;
    int    scene;
};

bool synth_engine_heard(const struct synth_engine *e, long latency,
                        struct synth_heard *out);

#endif
