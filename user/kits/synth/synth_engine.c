/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Synth Kit's engine (`synth_engine.h`): PulseMusic's `engine.lua`,
 * the part of it that makes sound and keeps time, in C.
 *
 * What stayed in Lua is what a person edits - the song as tables, saving it,
 * the performance a person records - and the window. What came here is
 * everything the audio thread does between one block and the next, in the
 * order `engine.lua` does it, so that the two could be rendered side by side
 * while this was written (`testing.md` 18.254).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "synth_engine.h"

#define PI 3.141592653589793

const char *const synth_fx_names[SYNTH_FX] = {
    "dTime", "dFb", "dDamp", "rSize", "rDamp", "duckRel", "dRet", "rRet",
};

/* ------------------------------------------------------------ the specs */

struct spec {
    double min, max;
    bool   exp, stepped;
};

/* `presets.lua`'s ranges, for automation: a lane holds 0 to 1. */
static const struct spec SYNTH_SPECS[SYNTH_PARAMS] = {
    { 1, 4, false, true }, { 1, 4, false, true }, { -2, 2, false, true },
    { 0, 50, false, false }, { 0, 1, false, false }, { 1, 3, false, true },
    { 0, 1, false, false }, { 0, 1, false, false }, { 1, 2, false, true },
    { 1, 3, false, true }, { 30, 18000, true, false }, { 0, 1, false, false },
    { 0, 1, false, false }, { 0.001, 8, true, false }, { 0.01, 4, true, false },
    { 0, 1, false, false }, { 0, 1, false, false }, { 0, 0.5, false, false },
    { 0.001, 4, true, false }, { 0.01, 4, true, false }, { 0, 1, false, false },
    { 0.005, 4, true, false }, { 0.05, 20, true, false }, { 0, 1, false, false },
    { 0, 1, false, false }, { 0, 1.5, false, false },
};

static const char *const ROW_NAMES[4] = { "tune", "decay", "tone", "level" };
static const struct spec ROW_SPECS[4] = {
    { -12, 12, false, false }, { 0.2, 3, true, false },
    { 0, 1, false, false }, { 0, 1.5, false, false },
};

static const char *const MIX_NAMES[SYNTH_MIX] = { "vol", "pan", "sendA", "sendB", "duck" };
static const struct spec MIX_SPECS[SYNTH_MIX] = {
    { 0, 1, false, false }, { -1, 1, false, false }, { 0, 1, false, false },
    { 0, 1, false, false }, { 0, 1, false, false },
};

static const struct spec FX_SPECS[SYNTH_FX] = {
    { 1, 3, false, true }, { 0, 0.9, false, false }, { 0, 1, false, false },
    { 0, 1, false, false }, { 0, 1, false, false }, { 0.04, 0.6, true, false },
    { 0, 1.5, false, false }, { 0, 1.5, false, false },
};

static int index_of(const char *const *names, int count, const char *name, size_t len)
{
    for (int i = 0; i < count; i++) {
        if (strlen(names[i]) == len && strncmp(names[i], name, len) == 0) {
            return i;
        }
    }

    return -1;
}

/* A number of one or two digits at `*at`, moving past it; 0 when none. */
static int number(const char **at)
{
    int n = 0, digits = 0;

    while (**at >= '0' && **at <= '9' && digits < 3) {
        n = n * 10 + (**at - '0');
        (*at)++;
        digits++;
    }

    return digits ? n : 0;
}

static void apply_spec(struct synth_target *t, const struct spec *s)
{
    t->min = s->min;
    t->max = s->max;
    t->exp = s->exp;
    t->stepped = s->stepped;
}

bool synth_target_parse(const char *name, struct synth_target *out)
{
    const char *at = name;

    memset(out, 0, sizeof *out);

    if (strncmp(at, "fx.", 3) == 0) {
        at += 3;

        int i = index_of(synth_fx_names, SYNTH_FX, at, strlen(at));

        if (i < 0) return false;

        out->kind = TARGET_FX;
        out->index = i;
        apply_spec(out, &FX_SPECS[i]);
        return true;
    }

    if (*at++ != 't') return false;

    int track = number(&at);

    if (track < 1 || track > SYNTH_TRACKS || *at++ != '.') return false;

    out->track = track - 1;

    if (at[0] == 'p' && at[1] == '.') {
        int i = index_of(synth_param_names, SYNTH_PARAMS, at + 2, strlen(at + 2));

        if (i < 0) return false;

        out->kind = TARGET_PARAM;
        out->index = i;
        apply_spec(out, &SYNTH_SPECS[i]);
        return true;
    }

    if (at[0] == 'm' && at[1] == '.') {
        int i = index_of(MIX_NAMES, SYNTH_MIX, at + 2, strlen(at + 2));

        if (i < 0) return false;

        out->kind = TARGET_MIX;
        out->index = i;
        apply_spec(out, &MIX_SPECS[i]);
        return true;
    }

    if (at[0] == 'r') {
        at++;

        int row = number(&at);

        if (row < 1 || row > SYNTH_ROWS || *at++ != '.') return false;

        int i = index_of(ROW_NAMES, 4, at, strlen(at));

        if (i < 0) return false;

        out->kind = TARGET_ROW;
        out->row = row - 1;
        out->index = i;
        apply_spec(out, &ROW_SPECS[i]);
        return true;
    }

    return false;
}

double synth_from_norm(const struct synth_target *t, double n)
{
    double v;

    if (n < 0) n = 0; else if (n > 1) n = 1;

    v = t->exp ? t->min * pow(t->max / t->min, n) : t->min + n * (t->max - t->min);

    if (t->stepped) v = floor(v + 0.5);

    return v;
}

double synth_to_norm(const struct synth_target *t, double v)
{
    if (t->exp) return log(v / t->min) / log(t->max / t->min);

    return (v - t->min) / (t->max - t->min);
}

double *synth_target_value(struct synth_song *song, const struct synth_target *t)
{
    if (t->kind == TARGET_FX) return &song->fx[t->index];

    struct synth_track *tr = &song->tracks[t->track];

    switch (t->kind) {
    case TARGET_PARAM:
        return tr->drum ? NULL : synth_param_at(&tr->params, t->index);
    case TARGET_ROW: {
        double *fields = &tr->rows[t->row].tune;

        return tr->drum ? &fields[t->index] : NULL;
    }
    case TARGET_MIX:
        switch (t->index) {
        case MIX_VOL:   return &tr->vol;
        case MIX_PAN:   return &tr->pan;
        case MIX_SENDA: return &tr->send_a;
        case MIX_SENDB: return &tr->send_b;
        default:        return &tr->duck;
        }
    default:
        return NULL;
    }
}

void synth_song_free(struct synth_song *song)
{
    if (song == NULL) return;

    for (int t = 0; t < SYNTH_TRACKS; t++) {
        for (int c = 0; c < SYNTH_SCENES; c++) {
            free(song->tracks[t].clips[c].notes);
        }
    }

    for (int s = 0; s < song->sections_count; s++) {
        for (int l = 0; l < song->sections[s].lanes_count; l++) {
            free(song->sections[s].lanes[l].values);
        }

        free(song->sections[s].lanes);
    }

    free(song->sections);
    free(song->targets);
    free(song->ramps);
    free(song->held);
    free(song);
}

/* ------------------------------------------------------------ automation */

static void auto_set(struct synth_engine *e, int target, double n)
{
    struct synth_song *song = e->song;
    const struct synth_target *t = &song->targets[target];
    double *at = synth_target_value(song, t);

    if (at != NULL) *at = synth_from_norm(t, n);
}

/* Every automated control back to where it rests: song start, stop, export. */
static void auto_restore(struct synth_engine *e)
{
    if (e->song == NULL) return;

    for (int i = 0; i < e->song->targets_count; i++) {
        e->song->ramps[i].on = false;
        e->song->held[i] = false;
    }

    for (int i = 0; i < e->song->targets_count; i++) {
        if (e->song->targets[i].has_base) auto_set(e, i, e->song->targets[i].base);
    }
}

static const struct synth_lane *lane_for(const struct synth_section *s, int target)
{
    for (int l = 0; l < s->lanes_count; l++) {
        if (s->lanes[l].target == target) return &s->lanes[l];
    }

    return NULL;
}

static bool lane_has_values(const struct synth_lane *lane)
{
    for (int i = 0; lane && i < lane->count; i++) {
        if (lane->values[i] >= 0) return true;
    }

    return false;
}

/* A new section: its holds end, and what it does not drive goes to rest. */
static void auto_section_start(struct synth_engine *e, const struct synth_section *s)
{
    for (int i = 0; i < e->song->targets_count; i++) e->song->held[i] = false;

    for (int i = 0; i < e->song->targets_count; i++) {
        const struct synth_target *t = &e->song->targets[i];

        if (t->has_base && !lane_has_values(lane_for(s, i))) {
            e->song->ramps[i].on = false;
            auto_set(e, i, t->base);
        }
    }
}

/* Once a 16th in a song: land on this step's value, glide to the next. */
static void auto_tick(struct synth_engine *e, long section_step, double dur)
{
    if (e->chain_pos < 1 || e->chain_pos > e->song->sections_count) return;

    const struct synth_section *s = &e->song->sections[e->chain_pos - 1];

    for (int l = 0; l < s->lanes_count; l++) {
        const struct synth_lane *lane = &s->lanes[l];
        int target = lane->target;

        if (e->song->held[target] || section_step < 0 || section_step >= lane->count) continue;

        double v = lane->values[section_step];

        if (v < 0) continue;

        auto_set(e, target, v);

        double next = (section_step + 1 < lane->count) ? lane->values[section_step + 1] : -1;

        if (next >= 0 && next != v && !e->song->targets[target].stepped) {
            e->song->ramps[target] = (struct synth_ramp){ true, v, next, 0, dur };
        } else {
            e->song->ramps[target].on = false;
        }
    }
}

static void auto_advance(struct synth_engine *e, int n)
{
    for (int i = 0; i < e->song->targets_count; i++) {
        struct synth_ramp *r = &e->song->ramps[i];

        if (!r->on) continue;

        r->pos += n;

        double t = r->pos / r->len;

        if (t >= 1) {
            auto_set(e, i, r->to);
            r->on = false;
        } else {
            auto_set(e, i, r->from + (r->to - r->from) * t);
        }
    }
}

/* ------------------------------------------------------------ notes */

static void trigger_drum(struct synth_engine *e, int ti, int row, double vel)
{
    struct synth_track *tr = &e->song->tracks[ti];
    struct synth_track_rt *rt = &e->rt[ti];
    const struct synth_drum_base *base = &synth_kits[tr->kit][row];

    /* A row stops what it was playing, and the closed hat the open one. */
    for (int i = 0; i < rt->dv_count; i++) {
        if (rt->dv[i].row == row || (row == 3 && rt->dv[i].row == 4)) {
            rt->dv[i].kill = true;
        }
    }

    if (rt->dv_count < SYNTH_DRUMVOICES) {
        synth_drum_start(&rt->dv[rt->dv_count++], row, base, vel);
    }

    rt->hits[row]++;

    if (base->type == DRUM_KICK) e->duck_env = 1;
}

void synth_engine_note_on(struct synth_engine *e, int ti, int pitch, double vel)
{
    struct synth_track *tr = &e->song->tracks[ti];
    struct synth_voice *vs = e->rt[ti].voices;

    if (tr->drum) {
        if (pitch >= 1 && pitch <= 8) trigger_drum(e, ti, pitch - 1, vel);
        return;
    }

    if ((int)tr->params.mono == 2) {
        synth_voice_on(&vs[0], pitch, vel, vs[0].active && vs[0].gate, &e->age, &e->rnd);
        return;
    }

    struct synth_voice *best = NULL;

    for (int i = 0; i < 8 && !best; i++) {
        if (vs[i].active && vs[i].note == pitch) best = &vs[i];
    }

    for (int i = 0; i < 8 && !best; i++) {
        if (!vs[i].active) best = &vs[i];
    }

    if (!best) {
        best = &vs[0];

        for (int i = 0; i < 8; i++) {
            if (vs[i].age < best->age) best = &vs[i];
        }
    }

    synth_voice_on(best, pitch, vel, false, &e->age, &e->rnd);
}

void synth_engine_note_off(struct synth_engine *e, int ti, int pitch)
{
    for (int i = 0; i < 8; i++) {
        struct synth_voice *v = &e->rt[ti].voices[i];

        if (v->active && v->gate && v->note == pitch) synth_voice_off(v);
    }
}

static void release_track(struct synth_engine *e, int ti)
{
    for (int i = 0; i < 8; i++) {
        struct synth_voice *v = &e->rt[ti].voices[i];

        if (v->active && v->gate) synth_voice_off(v);
    }

    e->rt[ti].offs_count = 0;
}

void synth_engine_bend(struct synth_engine *e, int ti, double semitones)
{
    e->rt[ti].bend = semitones;
}

void synth_engine_hold(struct synth_engine *e, int target, bool held)
{
    if (e->song && target >= 0 && target < e->song->targets_count) {
        e->song->held[target] = held;

        if (held) e->song->ramps[target].on = false;
    }
}

/* ------------------------------------------------------------ transport */

void synth_engine_launch_scene(struct synth_engine *e, int scene)
{
    e->pending_scene = scene;

    for (int ti = 0; ti < SYNTH_TRACKS; ti++) {
        e->rt[ti].queued = e->song->tracks[ti].clips[scene - 1].present ? scene : 0;
    }
}

void synth_engine_play(struct synth_engine *e)
{
    if (e->playing || e->song == NULL) return;

    e->step = 0;
    e->until_tick = 0;
    e->ticks_count = 0;
    e->finished = false;

    bool any = false;

    for (int ti = 0; ti < SYNTH_TRACKS; ti++) {
        struct synth_track_rt *rt = &e->rt[ti];

        rt->clip_start = 0;

        if (e->song_mode) {
            rt->playing = 0;
            rt->queued = -1;
        }

        if (rt->playing || rt->queued > 0) any = true;
    }

    if (e->song_mode) {
        e->chain_pos = 0;
        e->chain_left = 0;
        e->section_start = 0;
        auto_restore(e);
    } else if (!any) {
        synth_engine_launch_scene(e, 1);
    }

    e->playing = true;
}

void synth_engine_stop(struct synth_engine *e)
{
    if (e->playing && e->song_mode) auto_restore(e);

    e->playing = false;

    for (int i = 0; e->song && i < e->song->targets_count; i++) e->song->ramps[i].on = false;

    for (int ti = 0; ti < SYNTH_TRACKS; ti++) {
        release_track(e, ti);
        e->rt[ti].queued = -1;
    }

    e->ticks_count = 0;
}

void synth_engine_mode(struct synth_engine *e, bool song_mode)
{
    if (e->song_mode != song_mode) {
        synth_engine_stop(e);
        e->song_mode = song_mode;
    }
}

void synth_engine_launch_clip(struct synth_engine *e, int ti, int scene)
{
    e->rt[ti].queued = scene;

    if (!e->playing) synth_engine_play(e);
}

void synth_engine_stop_clip(struct synth_engine *e, int ti)
{
    e->rt[ti].queued = 0;
}

static double step_dur(const struct synth_engine *e, long s)
{
    double base = SYNTH_RATE * 60.0 / e->song->bpm / 4;
    double sw = e->song->swing;

    return (s % 2 == 0) ? base * (1 + sw) : base * (1 - sw);
}

static void tick(struct synth_engine *e)
{
    long s = e->step;
    struct synth_song *song = e->song;

    if (s % 16 == 0) {
        if (e->song_mode) {
            if (e->chain_left <= 0) {
                e->chain_pos++;

                if (e->chain_pos > song->sections_count) {
                    e->finished = true;
                    synth_engine_stop(e);
                    return;
                }

                const struct synth_section *sec = &song->sections[e->chain_pos - 1];

                synth_engine_launch_scene(e, sec->scene);
                e->chain_left = sec->bars;
                e->section_start = s;
                auto_section_start(e, sec);
            }

            e->chain_left--;
        }

        for (int ti = 0; ti < SYNTH_TRACKS; ti++) {
            struct synth_track_rt *rt = &e->rt[ti];

            if (rt->queued >= 0) {
                release_track(e, ti);
                rt->playing = rt->queued > 0 ? rt->queued : 0;
                rt->clip_start = s;
                rt->queued = -1;
            }
        }
    }

    for (int ti = 0; ti < SYNTH_TRACKS; ti++) {
        struct synth_track_rt *rt = &e->rt[ti];
        struct synth_track *tr = &song->tracks[ti];

        for (int i = rt->offs_count - 1; i >= 0; i--) {
            if (rt->offs[i].at <= s) {
                synth_engine_note_off(e, ti, rt->offs[i].pitch);
                memmove(&rt->offs[i], &rt->offs[i + 1],
                        (size_t)(rt->offs_count - i - 1) * sizeof rt->offs[0]);
                rt->offs_count--;
            }
        }

        const struct synth_clip *clip = rt->playing ? &tr->clips[rt->playing - 1] : NULL;

        if (clip == NULL || !clip->present || clip->len <= 0) continue;

        int pos = (int)((s - rt->clip_start) % clip->len);

        if (tr->drum) {
            for (int r = 0; r < SYNTH_ROWS; r++) {
                double vel = clip->steps[r][pos];

                if (vel > 0) trigger_drum(e, ti, r, vel);
            }
        } else {
            for (int n = 0; n < clip->notes_count; n++) {
                const struct synth_note *nt = &clip->notes[n];

                if (nt->step != pos) continue;

                synth_engine_note_on(e, ti, nt->pitch, nt->vel);

                if (rt->offs_count < SYNTH_OFFS) {
                    rt->offs[rt->offs_count++] = (struct synth_offs){ nt->pitch, s + nt->len };
                }
            }
        }
    }

    double d = step_dur(e, s);

    if (e->song_mode) auto_tick(e, s - e->section_start, d);

    if (e->ticks_count == SYNTH_TICKS) {
        memmove(&e->ticks[0], &e->ticks[1], (SYNTH_TICKS - 1) * sizeof e->ticks[0]);
        e->ticks_count--;
    }

    e->ticks[e->ticks_count++] = (struct synth_tick){
        e->total, s, d, e->chain_pos, s - e->section_start,
    };

    e->step = s + 1;
    e->until_tick += d;
}

bool synth_engine_heard(const struct synth_engine *e, long latency, double *step,
                        int *chain_pos, long *section_step)
{
    long h = e->total - latency;

    for (int i = e->ticks_count - 1; i >= 0; i--) {
        const struct synth_tick *tk = &e->ticks[i];

        if (tk->total <= h) {
            double frac = (double)(h - tk->total) / tk->dur;

            *step = (double)tk->step + (frac < 0.999 ? frac : 0.999);
            *chain_pos = tk->chain_pos;
            *section_step = tk->section_step;
            return true;
        }
    }

    return false;
}

/* ------------------------------------------------------------ rendering */

static void render_segment(struct synth_engine *e, double *l, double *r, int off, int n)
{
    struct synth_song *song = e->song;
    const double *fx = song->fx;
    int e1 = off + n;

    auto_advance(e, n);

    /* The duck: a kick sets it, and it lets go at `duckRel`. */
    double d_env = e->duck_env, dk = e->duck;
    double dc = exp(-1.0 / (fx[FX_DUCKREL] * SYNTH_RATE));

    for (int i = off; i < e1; i++) {
        dk += (d_env - dk) * 0.02;
        e->duck_buf[i] = dk;
        d_env *= dc;
        l[i] = r[i] = e->send_a[i] = e->send_b[i] = 0;
    }

    e->duck_env = d_env;
    e->duck = dk;

    bool any_solo = false;

    for (int ti = 0; ti < SYNTH_TRACKS; ti++) {
        if (song->tracks[ti].solo) any_solo = true;
    }

    for (int ti = 0; ti < SYNTH_TRACKS; ti++) {
        struct synth_track *tr = &song->tracks[ti];
        struct synth_track_rt *rt = &e->rt[ti];
        bool active = false;

        for (int i = off; i < e1; i++) e->tbuf[i] = 0;

        if (tr->drum) {
            for (int i = rt->dv_count - 1; i >= 0; i--) {
                struct synth_drum_voice *v = &rt->dv[i];

                synth_drum_render(v, &synth_kits[tr->kit][v->row], &tr->rows[v->row],
                                  e->tbuf, e->tmp, off, n, &e->rnd);

                if (v->dead) {
                    memmove(&rt->dv[i], &rt->dv[i + 1],
                            (size_t)(rt->dv_count - i - 1) * sizeof rt->dv[0]);
                    rt->dv_count--;
                }

                active = true;
            }
        } else {
            const struct synth_params *p = &tr->params;
            double lfo_inc = p->lfoR / SYNTH_RATE;

            rt->bend_s += (rt->bend - rt->bend_s) * 0.6;

            for (int v = 0; v < 8; v++) {
                if (rt->voices[v].active) {
                    synth_voice_render(&rt->voices[v], p, e->tbuf, off, n, rt->lfo_ph,
                                       lfo_inc, rt->bend_s, &e->rnd);
                    active = true;
                }
            }

            rt->lfo_ph = fmod(rt->lfo_ph + lfo_inc * n, 1.0);
        }

        if (!active) continue;

        double g = tr->vol * tr->vol * 1.6;

        if (tr->mute || (any_solo && !tr->solo)) g = 0;

        double a = (tr->pan + 1) * PI / 4;
        double gl = g * cos(a) * 1.414, gr = g * sin(a) * 1.414;
        double sa = g * tr->send_a, sb = g * tr->send_b, du = tr->duck, pk = rt->peak;

        for (int i = off; i < e1; i++) {
            double s = e->tbuf[i] * (1 - du * e->duck_buf[i]);
            double m = fabs(s * g);

            l[i] += s * gl;
            r[i] += s * gr;
            e->send_a[i] += s * sa;
            e->send_b[i] += s * sb;

            if (m > pk) pk = m;
        }

        rt->peak = pk;
    }

    double step_s = SYNTH_RATE * 60.0 / song->bpm / 4;

    synth_delay_process(&e->delay, e->send_a, l, r, off, n,
                        (int)floor(step_s * (fx[FX_DTIME] + 1)), fx[FX_DFB],
                        fx[FX_DDAMP], fx[FX_DRET]);
    synth_reverb_process(&e->reverb, e->send_b, l, r, off, n, fx[FX_RSIZE],
                         fx[FX_RDAMP], fx[FX_RRET]);

    double mg = song->master * song->master * 1.5, pl = e->peak_l, pr = e->peak_r;

    for (int i = off; i < e1; i++) {
        double lv = l[i] * mg, rv = r[i] * mg;

        if (fabs(lv) > pl) pl = fabs(lv);
        if (fabs(rv) > pr) pr = fabs(rv);

        l[i] = tanh(lv);
        r[i] = tanh(rv);
    }

    e->peak_l = pl;
    e->peak_r = pr;
}

void synth_engine_render(struct synth_engine *e, double *l, double *r, int n)
{
    if (e->song == NULL) {
        memset(l, 0, (size_t)n * sizeof *l);
        memset(r, 0, (size_t)n * sizeof *r);
        return;
    }

    int done = 0;

    while (done < n) {
        int seg = n - done;

        if (e->playing) {
            if (e->until_tick <= 0) tick(e);

            if (e->playing) {
                int until = (int)ceil(e->until_tick);

                if (until < seg) seg = until;
                if (seg < 1) seg = 1;
            }
        }

        render_segment(e, l, r, done, seg);
        done += seg;
        e->total += seg;

        if (e->playing) e->until_tick -= seg;
    }
}

/* ------------------------------------------------------------ setting up */

void synth_engine_init(struct synth_engine *e, uint64_t seed)
{
    memset(e, 0, sizeof *e);
    synth_random_seed(&e->rnd, seed);
    synth_delay_clear(&e->delay);
    synth_reverb_clear(&e->reverb);

    for (int ti = 0; ti < SYNTH_TRACKS; ti++) {
        e->rt[ti].queued = -1;

        for (int v = 0; v < 8; v++) e->rt[ti].voices[v].note = 60;
    }
}

struct synth_song *synth_engine_set_song(struct synth_engine *e,
                                         struct synth_song *song, bool keep)
{
    struct synth_song *old = e->song;

    if (!keep) {
        synth_engine_stop(e);

        for (int ti = 0; ti < SYNTH_TRACKS; ti++) {
            struct synth_track_rt *rt = &e->rt[ti];

            memset(rt, 0, sizeof *rt);
            rt->queued = -1;

            for (int v = 0; v < 8; v++) rt->voices[v].note = 60;
        }
    }

    e->song = song;

    if (e->chain_pos > (song ? song->sections_count : 0)) e->chain_pos = 0;

    return old;
}
