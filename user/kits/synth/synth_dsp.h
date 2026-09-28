/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Synth Kit's sound: a synthesiser voice, seven drums, a ping-pong delay
 * and a reverb (`roadmap.md` 6zh, Groove).
 *
 * Converted from PulseMusic's `dsp.lua`, Diego's LÖVE application, line for
 * line and in doubles, so the sound is the one he made: the same oscillators
 * with their polyBLEP corners, the same state-variable filter, the same
 * drum models and the same Freeverb. What changed is the language, since a
 * loop over samples on a deadline is C here (`CLAUDE.md`), and the noise:
 * one generator of the kit's own, seeded, where the original asked Lua's.
 *
 * Pure: no Lua, no `sys`, no allocation after a buffer is made - so the
 * audio thread never waits on the heap, and `tools/test_synth.c` runs all of
 * it on the Mac.
 */
#ifndef KOSMOS_SYNTH_DSP_H
#define KOSMOS_SYNTH_DSP_H

#include <stdbool.h>
#include <stdint.h>

#define SYNTH_RATE      44100
#define SYNTH_BLOCK     1024        /* frames the engine renders at a time */

/* The noise: xorshift64*, one per engine, uniform in [0, 1). */
struct synth_random {
    uint64_t state;
};

void   synth_random_seed(struct synth_random *r, uint64_t seed);
double synth_random(struct synth_random *r);

/*
 * A synthesiser's parameters, as PulseMusic names them (`presets.lua`).
 * Choices are held as their numbers - `w1` 1 to 4 for saw, square, triangle
 * and sine - and everything is a double, as it was.
 */
struct synth_params {
    double w1, w2, oct2, det, mix, uni, sub, noise, mono;
    double ftype, cut, res, env, fA, fD, fS, drive, glide;
    double aA, aD, aS, aR, lfoR, lfoC, lfoP, lvl;
};

#define SYNTH_PARAMS    26

/* The names above in their order, for a song read from Lua and automation. */
extern const char *const synth_param_names[SYNTH_PARAMS];

double *synth_param_at(struct synth_params *p, int index);

struct synth_voice {
    bool   active, gate;
    int    note;
    double vel, f, ft;
    double ph[5], rat[5], ph2, phs;
    double ae, fe, ic1, ic2;
    int    as, fs;                      /* 1 attack, 2 decay, 3 release */
    uint64_t age;
};

void synth_voice_on(struct synth_voice *v, int note, double vel, bool legato,
                    uint64_t *age, struct synth_random *rnd);
void synth_voice_off(struct synth_voice *v);
void synth_voice_render(struct synth_voice *v, const struct synth_params *p,
                        double *buf, int off, int n, double lfo_ph,
                        double lfo_inc, double bend, struct synth_random *rnd);

/* A drum of a kit, as a kit describes it: which model and its numbers. */
enum synth_drum_type {
    DRUM_KICK, DRUM_TOM, DRUM_SNARE, DRUM_CLAP, DRUM_HAT, DRUM_RIM, DRUM_PERC
};

struct synth_drum_base {
    enum synth_drum_type type;
    double f, sweep, pd, dec, drive, click, g;  /* kick, tom */
    double tdec, hp;                            /* snare, hat */
    double fm, metal;                           /* hat */
    double ratio, bp;                           /* perc */
};

/* What a person turns on a row: tune, decay, tone, level. */
struct synth_row {
    double tune, decay, tone, level;
};

struct synth_drum_voice {
    int    row;
    enum synth_drum_type type;
    double vel;
    int    t;
    double a, a2, pe, ph, ph2, lp, ic1, ic2, kg;
    bool   kill, dead;
    double phs[6];
};

void synth_drum_start(struct synth_drum_voice *v, int row,
                      const struct synth_drum_base *b, double vel);
void synth_drum_render(struct synth_drum_voice *v, const struct synth_drum_base *b,
                       const struct synth_row *u, double *buf, double *tmp,
                       int off, int n, struct synth_random *rnd);

/* The four kits PulseMusic ships, eight rows each. */
#define SYNTH_KITS  4
extern const struct synth_drum_base synth_kits[SYNTH_KITS][8];

#define SYNTH_DELAY_LEN (SYNTH_RATE * 2)

struct synth_delay {
    double l[SYNTH_DELAY_LEN], r[SYNTH_DELAY_LEN];
    int    w;
    double lpl, lpr;
};

void synth_delay_clear(struct synth_delay *d);
void synth_delay_process(struct synth_delay *d, const double *in, double *l,
                         double *r, int off, int n, int dsamp, double fb,
                         double damp, double ret);

#define SYNTH_COMBS 8
#define SYNTH_APS   4

struct synth_comb {
    double b[1617 + 23];
    int    len, idx;
    double st;
};

struct synth_allpass {
    double b[556 + 23];
    int    len, idx;
};

struct synth_reverb {
    struct synth_comb    combs[2][SYNTH_COMBS];
    struct synth_allpass aps[2][SYNTH_APS];
    double tmp[SYNTH_BLOCK];
};

void synth_reverb_clear(struct synth_reverb *rv);
void synth_reverb_process(struct synth_reverb *rv, const double *in, double *l,
                          double *r, int off, int n, double size, double damp,
                          double ret);

#endif
