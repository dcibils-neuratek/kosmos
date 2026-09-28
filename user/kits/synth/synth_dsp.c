/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Synth Kit's sound (`synth_dsp.h`): PulseMusic's `dsp.lua` in C.
 *
 * Each function keeps the shape of the Lua it came from - the same locals,
 * the same order of operations, the same constants - so the two can be read
 * side by side, and so the one check that matters could be made while it
 * was being written: rendered against the original, block by block
 * (`testing.md` 18.254).
 */
#include <math.h>
#include <string.h>

#include "synth_dsp.h"

#define TWO_PI  6.283185307179586
#define PI      3.141592653589793
#define INV_SR  (1.0 / SYNTH_RATE)

/* ------------------------------------------------------------ the noise */

void synth_random_seed(struct synth_random *r, uint64_t seed)
{
    r->state = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

double synth_random(struct synth_random *r)
{
    uint64_t x = r->state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->state = x;

    return (double)((x * 0x2545f4914f6cdd1dULL) >> 11) * (1.0 / 9007199254740992.0);
}

/* ------------------------------------------------------------ parameters */

const char *const synth_param_names[SYNTH_PARAMS] = {
    "w1", "w2", "oct2", "det", "mix", "uni", "sub", "noise", "mono",
    "ftype", "cut", "res", "env", "fA", "fD", "fS", "drive", "glide",
    "aA", "aD", "aS", "aR", "lfoR", "lfoC", "lfoP", "lvl",
};

double *synth_param_at(struct synth_params *p, int index)
{
    double *all = &p->w1;

    return (index >= 0 && index < SYNTH_PARAMS) ? &all[index] : NULL;
}

/* ------------------------------------------------------------ oscillators */

static double blep(double t, double dt)
{
    if (t < dt) {
        t = t / dt;
        return t + t - t * t - 1;
    }

    if (t > 1 - dt) {
        t = (t - 1) / dt;
        return t * t + t + t + 1;
    }

    return 0;
}

static double osc(int w, double t, double dt)
{
    if (w == 1) {
        return 2 * t - 1 - blep(t, dt);
    }

    if (w == 2) {
        double t2 = t + 0.5;

        if (t2 >= 1) {
            t2 -= 1;
        }

        return (t < 0.5 ? 1 : -1) + blep(t, dt) - blep(t2, dt);
    }

    if (w == 3) {
        return 4 * fabs(t - 0.5) - 1;
    }

    return sin(TWO_PI * t);
}

/* ------------------------------------------------------------ a voice */

static const double SPREAD[3][5] = {
    { 0 }, { -1, 0, 1 }, { -1, -0.5, 0, 0.5, 1 },
};

void synth_voice_on(struct synth_voice *v, int note, double vel, bool legato,
                    uint64_t *age, struct synth_random *rnd)
{
    double f = 440.0 * pow(2.0, (note - 69) / 12.0);

    v->note = note;
    v->ft = f;

    if (!legato) {
        v->f = f;
        v->vel = vel;
        v->as = 1;
        v->fs = 1;
        v->fe = 0;

        if (!v->active) {
            v->ae = 0;
            v->ic1 = 0;
            v->ic2 = 0;

            for (int u = 0; u < 5; u++) {
                v->ph[u] = synth_random(rnd);
            }

            v->ph2 = synth_random(rnd);
            v->phs = 0;
        }
    }

    *age += 1;
    v->active = true;
    v->gate = true;
    v->age = *age;
}

void synth_voice_off(struct synth_voice *v)
{
    v->gate = false;

    if (v->active) {
        v->as = 3;
        v->fs = 3;
    }
}

void synth_voice_render(struct synth_voice *v, const struct synth_params *p,
                        double *buf, int off, int n, double lfo_ph,
                        double lfo_inc, double bend, struct synth_random *rnd)
{
    int w1 = (int)p->w1, w2 = (int)p->w2;
    int which = (int)p->uni;

    if (which < 1) which = 1;
    if (which > 3) which = 3;

    int uni = which * 2 - 1;
    const double *spread = SPREAD[which - 1];
    double det = p->det;

    for (int u = 0; u < uni; u++) {
        v->rat[u] = pow(2.0, det * spread[u] / 1200.0);
    }

    double r2 = pow(2.0, p->oct2 + det / 1200.0);
    double mix = p->mix, sub = p->sub, noise = p->noise;
    double g1 = (1 - mix) / sqrt((double)uni);
    double a_inc = 1.0 / (fmax(p->aA, 0.0005) * SYNTH_RATE);
    double a_dc = exp(-1.0 / (p->aD * SYNTH_RATE * 0.35)), a_s = p->aS;
    double a_rc = exp(-1.0 / (p->aR * SYNTH_RATE * 0.35));
    double f_inc = 1.0 / (fmax(p->fA, 0.0005) * SYNTH_RATE);
    double f_dc = exp(-1.0 / (p->fD * SYNTH_RATE * 0.35)), f_s = p->fS;
    double k = 2 - 1.96 * p->res;
    double cut = p->cut, lfo_c = p->lfoC * 3, lfo_p = p->lfoP;
    int ftype = (int)p->ftype;
    double env_amt = p->env * 7 * (0.65 + 0.35 * v->vel);
    double kt = (v->note - 60) / 12.0 * 0.35;
    double glc = p->glide > 0 ? (1 - exp(-1.0 / (p->glide * SYNTH_RATE * 0.3))) : 1;
    double drive = p->drive;
    double dg = 1 + drive * 8, dn = 1 / sqrt(1 + drive * 8);
    double amp = p->lvl * (0.35 + 0.65 * v->vel);

    double *ph = v->ph, *rat = v->rat;
    double ph2 = v->ph2, phs = v->phs;
    double f = v->f, ft = v->ft;
    double ae = v->ae, fe = v->fe;
    int as = v->as, fs = v->fs;
    double ic1 = v->ic1, ic2 = v->ic2;
    double a1 = 0, a2 = 0, a3 = 0, pm = 1;

    for (int i = 0; i < n; i++) {
        if (i % 16 == 0) {
            double l = sin(TWO_PI * (lfo_ph + i * lfo_inc));
            double fc = cut * pow(2.0, env_amt * fe + lfo_c * l + kt);

            if (fc < 20) fc = 20; else if (fc > 18000) fc = 18000;

            double g = tan(PI * fc * INV_SR);

            a1 = 1 / (1 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
            pm = pow(2.0, (lfo_p * l + bend) / 12.0);
        }

        f = f + (ft - f) * glc;

        double dt = f * pm * INV_SR;
        double o = 0;

        for (int u = 0; u < uni; u++) {
            double d = dt * rat[u];

            if (d > 0.45) d = 0.45;

            double t = ph[u] + d;

            if (t >= 1) t -= 1;

            ph[u] = t;
            o += osc(w1, t, d);
        }

        double x = o * g1;

        if (mix > 0) {
            double d2 = dt * r2;

            if (d2 > 0.45) d2 = 0.45;

            ph2 += d2;

            if (ph2 >= 1) ph2 -= 1;

            x += osc(w2, ph2, d2) * mix;
        }

        if (sub > 0) {
            phs += dt * 0.5;

            if (phs >= 1) phs -= 1;

            x += sin(TWO_PI * phs) * sub;
        }

        if (noise > 0) {
            x += (synth_random(rnd) * 2 - 1) * noise;
        }

        /* The TPT state variable filter. */
        double v3 = x - ic2;
        double v1 = a1 * ic1 + a2 * v3;
        double v2 = ic2 + a2 * ic1 + a3 * v3;

        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;

        double y = (ftype == 1) ? v2 : (ftype == 2) ? v1 : x - k * v1 - v2;

        /* The envelopes. */
        if (as == 1) {
            ae += a_inc;
            if (ae >= 1) { ae = 1; as = 2; }
        } else if (as == 2) {
            ae = a_s + (ae - a_s) * a_dc;
        } else {
            ae *= a_rc;
        }

        if (fs == 1) {
            fe += f_inc;
            if (fe >= 1) { fe = 1; fs = 2; }
        } else if (fs == 2) {
            fe = f_s + (fe - f_s) * f_dc;
        } else {
            fe *= a_rc;
        }

        y *= ae;

        if (drive > 0) {
            y = tanh(y * dg) * dn;
        }

        buf[off + i] += y * amp;

        if (as == 3 && ae < 0.0002) {
            v->active = false;
            break;
        }
    }

    v->ph2 = ph2;
    v->phs = phs;
    v->f = f;
    v->ae = ae;
    v->as = as;
    v->fe = fe;
    v->fs = fs;
    v->ic1 = ic1;
    v->ic2 = ic2;
}

/* ------------------------------------------------------------ drums */

static const double HATF[6] = { 205.3, 304.4, 369.6, 522.7, 540, 800 };

#define K(f_, sw, pd_, dec_, dr, cl, g_) \
    { .type = DRUM_KICK, .f = f_, .sweep = sw, .pd = pd_, .dec = dec_, \
      .drive = dr, .click = cl, .g = g_ }
#define TOM(f_, sw, pd_, dec_, dr, cl, g_) \
    { .type = DRUM_TOM, .f = f_, .sweep = sw, .pd = pd_, .dec = dec_, \
      .drive = dr, .click = cl, .g = g_ }
#define SN(f_, td, dec_, hp_, g_) \
    { .type = DRUM_SNARE, .f = f_, .tdec = td, .dec = dec_, .hp = hp_, .g = g_ }
#define CL(f_, dec_, g_)    { .type = DRUM_CLAP, .f = f_, .dec = dec_, .g = g_ }
#define HAT(fm_, hp_, dec_, mt, g_) \
    { .type = DRUM_HAT, .fm = fm_, .hp = hp_, .dec = dec_, .metal = mt, .g = g_ }
#define RIM(f_, dec_, g_)   { .type = DRUM_RIM, .f = f_, .dec = dec_, .g = g_ }
#define PERC(f_, ra, bp_, dec_, g_) \
    { .type = DRUM_PERC, .f = f_, .ratio = ra, .bp = bp_, .dec = dec_, .g = g_ }

/* `presets.lua`'s four kits, row for row. */
const struct synth_drum_base synth_kits[SYNTH_KITS][8] = {
    {   /* 909 TECHNO */
        K(50, 250, 0.028, 0.13, 0.55, 0.35, 1.0), SN(190, 0.06, 0.13, 1800, 0.6),
        CL(1150, 0.11, 0.62), HAT(1.35, 8000, 0.03, 0.45, 0.4),
        HAT(1.35, 7000, 0.22, 0.45, 0.36), TOM(105, 90, 0.05, 0.18, 0.2, 0.05, 0.7),
        RIM(1750, 0.012, 0.5), PERC(410, 1.51, 2100, 0.09, 0.7),
    },
    {   /* 808 DEEP */
        K(45, 110, 0.045, 0.42, 0.1, 0.1, 1.0), SN(170, 0.09, 0.17, 1200, 0.6),
        CL(1000, 0.16, 0.7), HAT(1.0, 7000, 0.035, 0.8, 0.42),
        HAT(1.0, 6500, 0.32, 0.8, 0.38), TOM(90, 60, 0.06, 0.3, 0.05, 0, 0.7),
        RIM(1650, 0.01, 0.5), PERC(540, 1.481, 2640, 0.18, 0.8),
    },
    {   /* HOUSE CLASSIC */
        K(54, 190, 0.035, 0.17, 0.3, 0.3, 1.0), SN(200, 0.05, 0.12, 2200, 0.55),
        CL(1250, 0.14, 0.66), HAT(1.2, 8500, 0.028, 0.35, 0.38),
        HAT(1.2, 7500, 0.2, 0.35, 0.36), TOM(120, 80, 0.05, 0.2, 0.1, 0.05, 0.7),
        RIM(1850, 0.011, 0.5), PERC(620, 1.34, 3000, 0.06, 0.7),
    },
    {   /* INDUSTRIAL */
        K(47, 420, 0.02, 0.2, 1.0, 0.6, 0.95), SN(150, 0.05, 0.22, 900, 0.65),
        CL(850, 0.2, 0.8), HAT(1.7, 9000, 0.02, 0.7, 0.4),
        HAT(1.7, 6000, 0.28, 0.7, 0.36), TOM(80, 200, 0.04, 0.25, 0.8, 0.2, 0.7),
        RIM(1300, 0.02, 0.55), PERC(300, 1.72, 1500, 0.14, 0.8),
    },
};

static double coef(double sec)
{
    return exp(-1.0 / (fmax(sec, 0.0005) * SYNTH_RATE));
}

void synth_drum_start(struct synth_drum_voice *v, int row,
                      const struct synth_drum_base *b, double vel)
{
    memset(v, 0, sizeof *v);
    v->row = row;
    v->type = b->type;
    v->vel = vel;
    v->a = 1;
    v->a2 = 1;
    v->pe = 1;
    v->kg = 1;
}

static void kick(struct synth_drum_voice *v, const struct synth_drum_base *b,
                 const struct synth_row *u, double *tmp, int n,
                 struct synth_random *rnd)
{
    double tm = pow(2.0, u->tune / 12), tone = u->tone;
    double f0 = b->f * tm, sweep = b->sweep * tm * (0.5 + tone);
    double pc = coef(b->pd), ac = coef(b->dec * u->decay);
    double dr = 1 + b->drive * 6 * (0.3 + tone * 1.4);
    double dn = 1 / tanh(dr);
    double click = b->click * tone * 2;
    double a = v->a, pe = v->pe, ph = v->ph;
    int t = v->t;

    for (int i = 0; i < n; i++) {
        ph += (f0 + sweep * pe) * INV_SR;

        if (ph >= 1) ph -= 1;

        double s = tanh(sin(TWO_PI * ph) * dr) * dn;

        if (t < 180) s += (synth_random(rnd) * 2 - 1) * click * (1 - t / 180.0);

        double at = t < 40 ? t / 40.0 : 1;

        tmp[i] = s * a * at;
        a *= ac;
        pe *= pc;
        t++;
    }

    v->a = a;
    v->pe = pe;
    v->ph = ph;
    v->t = t;

    if (a < 0.0004) v->dead = true;
}

static void snare(struct synth_drum_voice *v, const struct synth_drum_base *b,
                  const struct synth_row *u, double *tmp, int n,
                  struct synth_random *rnd)
{
    double tm = pow(2.0, u->tune / 12), tone = u->tone;
    double f1 = b->f * tm;
    double tc = coef(b->tdec * u->decay), nc = coef(b->dec * u->decay), pc = coef(0.012);
    double tl = (1 - tone) * 1.3, nl = tone * 1.7;
    double hc = 1 - exp(-TWO_PI * b->hp * INV_SR);
    double a = v->a, a2 = v->a2, pe = v->pe, ph = v->ph, ph2 = v->ph2, lp = v->lp;

    for (int i = 0; i < n; i++) {
        double fr = f1 * (1 + 0.6 * pe);

        ph += fr * INV_SR;
        if (ph >= 1) ph -= 1;
        ph2 += fr * 1.78 * INV_SR;
        if (ph2 >= 1) ph2 -= 1;

        double x = synth_random(rnd) * 2 - 1;

        lp += (x - lp) * hc;
        tmp[i] = (sin(TWO_PI * ph) + 0.5 * sin(TWO_PI * ph2)) * a * tl
                 + (x - lp) * a2 * nl;
        a *= tc;
        a2 *= nc;
        pe *= pc;
    }

    v->a = a;
    v->a2 = a2;
    v->pe = pe;
    v->ph = ph;
    v->ph2 = ph2;
    v->lp = lp;

    if (a2 < 0.0004 && a < 0.0004) v->dead = true;
}

static void clap(struct synth_drum_voice *v, const struct synth_drum_base *b,
                 const struct synth_row *u, double *tmp, int n,
                 struct synth_random *rnd)
{
    double fc = b->f * pow(2.0, u->tune / 12) * (0.7 + u->tone * 0.6);
    double g = tan(PI * fc * INV_SR), k = 0.45;
    double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * g * a1;
    double ac = coef(b->dec * u->decay);
    double ic1 = v->ic1, ic2 = v->ic2, a = v->a;
    int t = v->t;

    for (int i = 0; i < n; i++) {
        double x = synth_random(rnd) * 2 - 1;
        double v3 = x - ic2;
        double v1 = a1 * ic1 + a2 * v3;
        double v2 = ic2 + a2 * ic1 + a3 * v3;
        double e;

        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;

        if (t < 1320) {
            e = exp(-(double)(t % 440) / 150);
        } else {
            e = a;
            a *= ac;
        }

        tmp[i] = v1 * e * 1.6;
        t++;
    }

    v->ic1 = ic1;
    v->ic2 = ic2;
    v->a = a;
    v->t = t;

    if (a < 0.0004) v->dead = true;
}

static void hat(struct synth_drum_voice *v, const struct synth_drum_base *b,
                const struct synth_row *u, double *tmp, int n,
                struct synth_random *rnd)
{
    double tm = pow(2.0, u->tune / 12) * b->fm;
    double fc = fmin(b->hp * (0.6 + u->tone * 0.8), 16000);
    double g = tan(PI * fc * INV_SR), k = 1.1;
    double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * g * a1;
    double ac = coef(b->dec * u->decay), metal = b->metal;
    double ic1 = v->ic1, ic2 = v->ic2, a = v->a;
    int t = v->t;
    double inc[6], p[6];

    for (int j = 0; j < 6; j++) {
        inc[j] = HATF[j] * tm * INV_SR;
        p[j] = v->phs[j];
    }

    for (int i = 0; i < n; i++) {
        double m = 0;

        for (int j = 0; j < 6; j++) {
            p[j] += inc[j];
            if (p[j] >= 1) p[j] -= 1;
            m += p[j] < 0.5 ? 1 : -1;
        }

        double x = m * 0.1667 * metal + (synth_random(rnd) * 2 - 1) * (1 - metal);
        double v3 = x - ic2;
        double v1 = a1 * ic1 + a2 * v3;
        double v2 = ic2 + a2 * ic1 + a3 * v3;

        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;

        double at = t < 20 ? t / 20.0 : 1;

        tmp[i] = (x - k * v1 - v2) * a * at * 2.2;
        a *= ac;
        t++;
    }

    for (int j = 0; j < 6; j++) v->phs[j] = p[j];

    v->ic1 = ic1;
    v->ic2 = ic2;
    v->a = a;
    v->t = t;

    if (a < 0.0004) v->dead = true;
}

static void rim(struct synth_drum_voice *v, const struct synth_drum_base *b,
                const struct synth_row *u, double *tmp, int n,
                struct synth_random *rnd)
{
    double f1 = b->f * pow(2.0, u->tune / 12);
    double ac = coef(b->dec * u->decay);
    double dr = 1 + u->tone * 5;
    double a = v->a, ph = v->ph, ph2 = v->ph2;
    int t = v->t;

    for (int i = 0; i < n; i++) {
        ph += f1 * INV_SR;
        if (ph >= 1) ph -= 1;
        ph2 += f1 * 0.283 * INV_SR;
        if (ph2 >= 1) ph2 -= 1;

        double s = sin(TWO_PI * ph) + 0.7 * sin(TWO_PI * ph2);

        if (t < 50) s += (synth_random(rnd) * 2 - 1) * (1 - t / 50.0);

        tmp[i] = tanh(s * a * dr) * 0.8;
        a *= ac;
        t++;
    }

    v->a = a;
    v->ph = ph;
    v->ph2 = ph2;
    v->t = t;

    if (a < 0.0004) v->dead = true;
}

static void perc(struct synth_drum_voice *v, const struct synth_drum_base *b,
                 const struct synth_row *u, double *tmp, int n)
{
    double tm = pow(2.0, u->tune / 12);
    double f1 = b->f * tm, f2 = b->f * tm * b->ratio;
    double fc = fmin(b->bp * tm * (0.6 + u->tone * 0.8), 15000);
    double g = tan(PI * fc * INV_SR), k = 0.6;
    double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * g * a1;
    double c1 = coef(0.012), c2 = coef(b->dec * u->decay);
    double a = v->a, e2 = v->a2, ph = v->ph, ph2 = v->ph2, ic1 = v->ic1, ic2 = v->ic2;

    for (int i = 0; i < n; i++) {
        ph += f1 * INV_SR;
        if (ph >= 1) ph -= 1;
        ph2 += f2 * INV_SR;
        if (ph2 >= 1) ph2 -= 1;

        double x = ((ph < 0.5 ? 1 : -1) + (ph2 < 0.5 ? 1 : -1)) * 0.5;
        double v3 = x - ic2;
        double v1 = a1 * ic1 + a2 * v3;
        double v2 = ic2 + a2 * ic1 + a3 * v3;

        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        tmp[i] = v1 * (0.6 * a + 0.4 * e2);
        a *= c1;
        e2 *= c2;
    }

    v->a = a;
    v->a2 = e2;
    v->ph = ph;
    v->ph2 = ph2;
    v->ic1 = ic1;
    v->ic2 = ic2;

    if (e2 < 0.0004) v->dead = true;
}

void synth_drum_render(struct synth_drum_voice *v, const struct synth_drum_base *b,
                       const struct synth_row *u, double *buf, double *tmp,
                       int off, int n, struct synth_random *rnd)
{
    switch (v->type) {
    case DRUM_KICK:
    case DRUM_TOM:   kick(v, b, u, tmp, n, rnd); break;
    case DRUM_SNARE: snare(v, b, u, tmp, n, rnd); break;
    case DRUM_CLAP:  clap(v, b, u, tmp, n, rnd); break;
    case DRUM_HAT:   hat(v, b, u, tmp, n, rnd); break;
    case DRUM_RIM:   rim(v, b, u, tmp, n, rnd); break;
    case DRUM_PERC:  perc(v, b, u, tmp, n); break;
    }

    double g = b->g * u->level * (0.25 + 0.75 * v->vel * v->vel) * 0.75;

    if (v->kill) {
        double kg = v->kg;

        for (int i = 0; i < n; i++) {
            buf[off + i] += tmp[i] * g * kg;
            kg *= 0.985;
        }

        v->kg = kg;

        if (kg < 0.001) v->dead = true;
    } else {
        for (int i = 0; i < n; i++) {
            buf[off + i] += tmp[i] * g;
        }
    }
}

/* ------------------------------------------------------------ the delay */

void synth_delay_clear(struct synth_delay *d)
{
    memset(d, 0, sizeof *d);
}

void synth_delay_process(struct synth_delay *d, const double *in, double *l,
                         double *r, int off, int n, int dsamp, double fb,
                         double damp, double ret)
{
    int len = SYNTH_DELAY_LEN, w = d->w;
    double lpl = d->lpl, lpr = d->lpr;
    double c = 1 - damp * 0.85;

    if (dsamp >= len) dsamp = len - 1;

    for (int i = off; i < off + n; i++) {
        int rd = w - dsamp;

        if (rd < 0) rd += len;

        double dl = d->l[rd], dr = d->r[rd];

        lpl += (dl - lpl) * c;
        lpr += (dr - lpr) * c;
        d->l[w] = in[i] + lpr * fb + 1e-20;
        d->r[w] = lpl * fb;
        l[i] += dl * ret;
        r[i] += dr * ret;

        if (++w >= len) w = 0;
    }

    d->w = w;
    d->lpl = lpl;
    d->lpr = lpr;
}

/* ------------------------------------------------------------ the reverb */

static const int COMB[SYNTH_COMBS] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const int AP[SYNTH_APS] = { 556, 441, 341, 225 };

void synth_reverb_clear(struct synth_reverb *rv)
{
    memset(rv, 0, sizeof *rv);

    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < SYNTH_COMBS; i++) rv->combs[c][i].len = COMB[i] + c * 23;
        for (int i = 0; i < SYNTH_APS; i++) rv->aps[c][i].len = AP[i] + c * 23;
    }
}

void synth_reverb_process(struct synth_reverb *rv, const double *in, double *l,
                          double *r, int off, int n, double size, double damp,
                          double ret)
{
    double fb = 0.7 + size * 0.28;
    double d1 = damp * 0.4, d2 = 1 - d1;
    double *tmp = rv->tmp;

    for (int c = 0; c < 2; c++) {
        double *out = c == 0 ? l : r;

        for (int i = 0; i < n; i++) tmp[i] = 0;

        for (int j = 0; j < SYNTH_COMBS; j++) {
            struct synth_comb *cb = &rv->combs[c][j];
            int len = cb->len, idx = cb->idx;
            double st = cb->st;

            for (int i = 0; i < n; i++) {
                double o = cb->b[idx];

                st = o * d2 + st * d1;
                cb->b[idx] = in[off + i] * 0.03 + st * fb + 1e-20;

                if (++idx >= len) idx = 0;

                tmp[i] += o;
            }

            cb->idx = idx;
            cb->st = st;
        }

        for (int j = 0; j < SYNTH_APS; j++) {
            struct synth_allpass *ap = &rv->aps[c][j];
            int len = ap->len, idx = ap->idx;

            for (int i = 0; i < n; i++) {
                double bo = ap->b[idx];
                double x = tmp[i];

                ap->b[idx] = x + bo * 0.5;
                tmp[i] = bo - x;

                if (++idx >= len) idx = 0;
            }

            ap->idx = idx;
        }

        for (int i = 0; i < n; i++) out[off + i] += tmp[i] * ret;
    }
}
