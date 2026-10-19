/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The GPU's execution units, emulated (`eu_emu.h`). Every field is read
 * where `eu.lua` writes it, which is where Mesa's `pre_xe.json` puts it;
 * the branches follow the Haswell PRM's pseudocode, which generation 9
 * kept: each lane has its own instruction pointer, PcIP, and runs an
 * instruction when its PcIP is the thread's.
 */

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "eu_emu.h"
#include "../user/include/gpuproto.h"

#define GRF_BYTES   (128u * 32u)
#define LANES       16u

/* A run of an instruction's 128 bits, `lo` to `hi`. */
static uint32_t bits(const uint32_t *w, unsigned lo, unsigned hi)
{
    uint32_t v = 0;
    unsigned b;

    for (b = hi + 1u; b-- > lo;) {
        v = (v << 1) | ((w[b / 32u] >> (b % 32u)) & 1u);
    }

    return v;
}

enum { T_UD = 0, T_D = 1, T_UW = 2, T_W = 3, T_UB = 4, T_B = 5, T_DF = 6, T_F = 7 };
enum { FILE_ARF = 0, FILE_GRF = 1, FILE_IMM = 3 };

static unsigned type_bytes(unsigned t)
{
    return t == T_UW || t == T_W ? 2u : t == T_UB || t == T_B ? 1u : t == T_DF ? 8u : 4u;
}

struct thread {
    uint8_t  grf[GRF_BYTES];
    uint16_t flag[2][2];            /* f0.0, f0.1, f1.0, f1.1 */
    uint32_t pcip[LANES];
    bool     broke[LANES];          /* left its loop by break: let out at the while */
    uint32_t ip;                    /* in instructions */
    unsigned simd;
};

/* A value as the execution units hold it: an integer or a float. */
struct val {
    bool f;
    double i;                       /* integers, exactly - every 32-bit one fits */
    float x;
};

static struct val ival(double i) { struct val v = { false, i, 0 }; return v; }
static struct val fval(float x) { struct val v = { true, 0, x }; return v; }

static struct val load(const uint8_t *at, unsigned type)
{
    uint32_t u = 0;

    memcpy(&u, at, type_bytes(type) > 4 ? 4 : type_bytes(type));

    switch (type) {
    case T_UD: return ival((double)u);
    case T_D:  return ival((double)(int32_t)u);
    case T_UW: return ival((double)(uint16_t)u);
    case T_W:  return ival((double)(int16_t)u);
    case T_UB: return ival((double)(uint8_t)u);
    case T_B:  return ival((double)(int8_t)u);
    default: { float x; memcpy(&x, &u, 4); return fval(x); }
    }
}

static float as_f(struct val v) { return v.f ? v.x : (float)v.i; }

/* An integer as the destination's type keeps it: its low bits. */
static uint32_t wrap(double i)
{
    double m = fmod(i, 4294967296.0);

    if (m < 0) {
        m += 4294967296.0;
    }

    return (uint32_t)m;
}

static void store(uint8_t *at, unsigned type, struct val v, bool saturate)
{
    uint32_t u;

    if (type == T_F) {
        float x = as_f(v);

        if (saturate) {
            x = x != x ? 0.0f : x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
        }

        memcpy(at, &x, 4);
        return;
    }

    if (v.f) {
        /* A float into an integer: toward zero, held to the type's range. */
        double d = v.x != v.x ? 0 : trunc((double)v.x);
        double lo = type == T_D ? -2147483648.0 : type == T_W ? -32768.0 : type == T_B ? -128.0 : 0;
        double hi = type == T_D ? 2147483647.0 : type == T_W ? 32767.0 : type == T_B ? 127.0
                  : type == T_UW ? 65535.0 : type == T_UB ? 255.0 : 4294967295.0;

        u = (uint32_t)(int64_t)(d < lo ? lo : d > hi ? hi : d);
    } else {
        u = wrap(v.i);
    }

    memcpy(at, &u, type_bytes(type));
}

/*--------------------------------------------------------------------------
 * Operands.
 *------------------------------------------------------------------------*/

static const unsigned VSTRIDE[] = { 0, 1, 2, 4, 8, 16, 32, 0 };
static const unsigned WIDTH[] = { 1, 2, 4, 8, 16, 0, 0, 0 };
static const unsigned HSTRIDE[] = { 0, 1, 2, 4 };

struct src {
    unsigned file, type, nr, sub, v, w, h;
    bool abs, neg;
    uint32_t imm;
};

/* An align1 source at `base` - 64 for src0, 96 for src1. */
static void src_at(const uint32_t *w, unsigned file, unsigned type, unsigned base, struct src *s)
{
    memset(s, 0, sizeof *s);
    s->file = file;
    s->type = type;

    if (file == FILE_IMM) {
        s->imm = bits(w, 96, 127);
        return;
    }

    s->sub = bits(w, base, base + 4);
    s->nr = bits(w, base + 5, base + 12);
    s->abs = bits(w, base + 13, base + 13) != 0;
    s->neg = bits(w, base + 14, base + 14) != 0;
    s->h = HSTRIDE[bits(w, base + 16, base + 17)];
    s->w = WIDTH[bits(w, base + 18, base + 20)];
    s->v = VSTRIDE[bits(w, base + 21, base + 24)];
}

static struct val modify(struct val v, const struct src *s)
{
    if (s->abs) {
        if (v.f) { v.x = fabsf(v.x); } else { v.i = fabs(v.i); }
    }

    if (s->neg) {
        if (v.f) { v.x = -v.x; } else { v.i = -v.i; }
    }

    return v;
}

static struct val read_src(const struct thread *t, const struct src *s, unsigned lane)
{
    struct val v;

    if (s->file == FILE_IMM) {
        if (s->type == T_DF) {      /* :v, eight signed nibbles a lane each */
            int n = (int)((s->imm >> (4u * (lane % 8u))) & 0xfu);

            return ival(n >= 8 ? n - 16 : n);
        }

        return load((const uint8_t *)&s->imm, s->type);
    }

    if (s->file == FILE_ARF) {
        return ival(0);             /* null: the only one a kernel reads here */
    }

    {
        unsigned es = type_bytes(s->type), w = s->w ? s->w : 1u;
        size_t at = (size_t)s->nr * 32u + s->sub + (size_t)(lane / w) * s->v * es + (size_t)(lane % w) * s->h * es;

        v = at + es <= GRF_BYTES ? load(&t->grf[at], s->type) : ival(0);
    }

    return modify(v, s);
}

/*--------------------------------------------------------------------------
 * Flags and predicates.
 *------------------------------------------------------------------------*/

static bool flag_bit(const struct thread *t, unsigned nr, unsigned sub, unsigned lane)
{
    return (t->flag[nr & 1u][sub & 1u] >> lane) & 1u;
}

static void set_flag(struct thread *t, unsigned nr, unsigned sub, unsigned lane, bool on)
{
    uint16_t m = (uint16_t)(1u << lane);

    t->flag[nr & 1u][sub & 1u] = on ? (uint16_t)(t->flag[nr & 1u][sub & 1u] | m)
                                    : (uint16_t)(t->flag[nr & 1u][sub & 1u] & ~m);
}

/* Lane by lane, or as a group - any16h, all8h - for every lane at once. */
static bool predicate(const struct thread *t, const uint32_t *w, unsigned lane)
{
    unsigned control = bits(w, 16, 19), nr = bits(w, 33, 33), sub = bits(w, 32, 32);
    bool inv = bits(w, 20, 20) != 0, on;

    if (control == 0) {
        return true;
    }

    if (control == 1) {
        on = flag_bit(t, nr, sub, lane);
    } else {
        unsigned group = control == 2 || control == 3 ? 2u : control == 4 || control == 5 ? 4u
                       : control == 6 || control == 7 ? 8u : control == 10 || control == 11 ? 16u : 32u;
        bool any = control == 2 || control == 4 || control == 6 || control == 10 || control == 12;
        unsigned first = (lane / group) * group, k, count = 0;

        for (k = first; k < first + group && k < LANES; k++) {
            count += flag_bit(t, nr, sub, k);
        }

        on = any ? count > 0 : count == group;
    }

    return inv ? !on : on;
}

static bool compare(unsigned cond, struct val a, struct val b)
{
    if (a.f || b.f) {
        float x = as_f(a), y = as_f(b);

        switch (cond) {
        case 1: return x == y;
        case 2: return x != y;
        case 3: return x > y;
        case 4: return x >= y;
        case 5: return x < y;
        case 6: return x <= y;
        default: return false;
        }
    }

    switch (cond) {
    case 1: return a.i == b.i;
    case 2: return a.i != b.i;
    case 3: return a.i > b.i;
    case 4: return a.i >= b.i;
    case 5: return a.i < b.i;
    case 6: return a.i <= b.i;
    default: return false;
    }
}

/*--------------------------------------------------------------------------
 * The arithmetic.
 *------------------------------------------------------------------------*/

static double as_u32(struct val v) { return (double)wrap(v.f ? trunc((double)v.x) : v.i); }
static double as_s32(struct val v) { return (double)(int32_t)wrap(v.f ? trunc((double)v.x) : v.i); }

static struct val alu(unsigned op, unsigned math, struct val a, struct val b, bool *ok)
{
    bool fl = a.f || b.f;

    switch (op) {
    case 0x01: return a;                                            /* mov */
    case 0x04: return ival((double)(~wrap(a.i)));                   /* not */
    case 0x05: return ival((double)(wrap(a.i) & wrap(b.i)));        /* and */
    case 0x06: return ival((double)(wrap(a.i) | wrap(b.i)));        /* or */
    case 0x07: return ival((double)(wrap(a.i) ^ wrap(b.i)));        /* xor */
    case 0x08: return ival((double)(wrap(a.i) >> (wrap(b.i) & 31u)));               /* shr */
    case 0x09: return ival((double)(uint32_t)(wrap(a.i) << (wrap(b.i) & 31u)));     /* shl */
    case 0x0c: return ival((double)((int32_t)wrap(a.i) >> (wrap(b.i) & 31u)));      /* asr */
    case 0x40: return fl ? fval(as_f(a) + as_f(b)) : ival(a.i + b.i);              /* add */
    case 0x41:                                                                      /* mul */
        if (fl) {
            return fval(as_f(a) * as_f(b));
        }
        /* Integers: the low 32 bits of the product, signed or not alike. */
        return ival((double)(uint32_t)((uint64_t)(int64_t)as_s32(a) * (uint64_t)(int64_t)as_s32(b)));
    case 0x43: { float x = as_f(a); return fval(x - floorf(x)); }   /* frc */
    case 0x44: return fval(ceilf(as_f(a)));                         /* rndu */
    case 0x45: return fval(floorf(as_f(a)));                        /* rndd */
    case 0x46: return fval(nearbyintf(as_f(a)));                    /* rnde */
    case 0x47: return fval(truncf(as_f(a)));                        /* rndz */
    case 0x38: {                                                    /* math */
        float x = as_f(a), y = as_f(b);

        switch (math) {
        case 1:  return fval(1.0f / x);
        case 2:  return fval(log2f(x));
        case 3:  return fval(exp2f(x));
        case 4:  return fval(sqrtf(x));
        case 5:  return fval(1.0f / sqrtf(x));
        case 6:  return fval(sinf(x));
        case 7:  return fval(cosf(x));
        case 9:  return fval(x / y);
        case 10: return fval(powf(x, y));
        default: *ok = false; return a;
        }
    }
    default:
        *ok = false;
        return a;
    }
}

static bool lane_on(const struct thread *t, const uint32_t *w, unsigned lane, unsigned exec)
{
    return lane < exec && (bits(w, 34, 34) != 0 || (t->pcip[lane] == t->ip && !t->broke[lane]));
}

/* An align1 instruction of one or two sources. */
static const char *two_source(struct thread *t, const uint32_t *w, unsigned op, unsigned exec,
                              struct eu_stats *st)
{
    unsigned dst_file = bits(w, 35, 36), dst_type = bits(w, 37, 40);
    unsigned dst_sub = bits(w, 48, 52), dst_nr = bits(w, 53, 60), dst_h = HSTRIDE[bits(w, 61, 62)];
    unsigned f0 = bits(w, 41, 42), f1 = bits(w, 89, 90);
    unsigned cond = bits(w, 24, 27), fnr = bits(w, 33, 33), fsub = bits(w, 32, 32);
    bool sat = bits(w, 31, 31) != 0;
    struct src s0, s1;
    struct val out[LANES];
    bool write[LANES], flag_out[LANES], raw[LANES] = { false }, has_flag = false, ok = true;
    unsigned lane;

    if (bits(w, 63, 63) || (f0 != FILE_IMM && bits(w, 79, 79)) || (f0 != FILE_IMM && f1 != FILE_IMM && bits(w, 111, 111))) {
        return "an indirect operand";
    }

    src_at(w, f0, bits(w, 43, 46), 64, &s0);
    src_at(w, f0 == FILE_IMM ? FILE_ARF : f1, f0 == FILE_IMM ? T_UD : bits(w, 91, 94), 96, &s1);

    if (f0 != FILE_IMM && f1 == FILE_IMM) {
        s1.file = FILE_IMM;
        s1.imm = bits(w, 96, 127);
    }

    for (lane = 0; lane < LANES; lane++) {
        bool on = lane_on(t, w, lane, exec);
        bool pred = on && predicate(t, w, lane);
        struct val a, b;

        write[lane] = false;
        flag_out[lane] = false;

        if (!on) {
            continue;
        }

        a = read_src(t, &s0, lane);
        b = read_src(t, &s1, lane);

        if (op == 0x10) {                               /* cmp: a flag, and the destination 0 or ~0 */
            bool c = compare(cond, a, b);

            /* All ones or nought, whatever the destination's type: a
             * float destination holds the bits, which Mesa then ands with
             * 1.0's to make a boolean a number. */
            if (pred) {
                flag_out[lane] = c;
                has_flag = true;
                out[lane] = ival(c ? 4294967295.0 : 0);
                raw[lane] = true;
                write[lane] = true;
            }

            continue;
        }

        if (op == 0x02) {                               /* sel: by the predicate, or by a condition */
            if (bits(w, 16, 19) == 0) {
                /* sel.l, sel.g and the rest: the first where it holds
                 * against the second, as Mesa makes min and max; the flag
                 * left as it was (`testing.md` 18.702). */
                if (cond == 0) {
                    return "sel with neither a predicate nor a condition";
                }

                out[lane] = compare(cond, a, b) ? a : b;
            } else {
                out[lane] = predicate(t, w, lane) ? a : b;
            }

            write[lane] = true;
            continue;
        }

        if (!pred) {
            continue;
        }

        /* An integer mul, shift or logic reads its sources as 32-bit words. */
        if (op == 0x05 || op == 0x06 || op == 0x07 || op == 0x04 || op == 0x08 || op == 0x09 || op == 0x0c) {
            a = s0.type == T_D || s0.type == T_W || s0.type == T_B ? ival(as_s32(a)) : ival(as_u32(a));
            b = s1.type == T_D || s1.type == T_W || s1.type == T_B ? ival(as_s32(b)) : ival(as_u32(b));
        }

        out[lane] = alu(op, bits(w, 24, 27), a, b, &ok);

        if (!ok) {
            return "an instruction this emulator does not run";
        }

        write[lane] = true;

        if (cond != 0 && op != 0x38) {
            flag_out[lane] = compare(cond, out[lane], out[lane].f ? fval(0) : ival(0));
            has_flag = true;
        }
    }

    for (lane = 0; lane < LANES; lane++) {
        if (write[lane]) {
            st->lanes++;

            if (dst_file == FILE_GRF) {
                size_t at = (size_t)dst_nr * 32u + dst_sub + (size_t)lane * dst_h * type_bytes(dst_type);

                if (at + type_bytes(dst_type) > GRF_BYTES) {
                    return "a destination past r127";
                }

                if (raw[lane]) {
                    uint32_t u = wrap(out[lane].i);

                    memcpy(&t->grf[at], &u, type_bytes(dst_type) > 4 ? 4 : type_bytes(dst_type));
                } else {
                    store(&t->grf[at], dst_type, out[lane], sat);
                }
            }
        }

        if (has_flag && lane_on(t, w, lane, exec) && (op != 0x10 || predicate(t, w, lane))) {
            set_flag(t, fnr, fsub, lane, flag_out[lane]);
        }
    }

    return NULL;
}

/* Three sources, align16: `mad`, dst = src0 + src1 * src2, fused. */
static const char *three_source(struct thread *t, const uint32_t *w, unsigned exec, struct eu_stats *st)
{
    unsigned dst_nr = bits(w, 56, 63), dst_sub = bits(w, 53, 55), lane, k;

    if (bits(w, 0, 6) != 0x5b) {
        return "a three-source instruction this emulator does not run";
    }

    if (bits(w, 46, 48) != 0 || bits(w, 43, 45) != 0) {
        return "a three-source type other than float";
    }

    for (lane = 0; lane < LANES; lane++) {
        float v[3];

        if (!lane_on(t, w, lane, exec) || !predicate(t, w, lane)) {
            continue;
        }

        for (k = 0; k < 3; k++) {
            unsigned base = 64 + k * 21;
            bool rep = bits(w, base, base) != 0;
            unsigned sub = bits(w, base + 9, base + 11) | (bits(w, base + 20, base + 20) << 3);
            unsigned nr = bits(w, base + 12, base + 19);
            size_t at = (size_t)nr * 32u + (size_t)sub * 4u + (rep ? 0u : lane * 4u);
            float x;

            memcpy(&x, &t->grf[at], 4);

            if (bits(w, 37 + k * 2, 37 + k * 2)) { x = fabsf(x); }
            if (bits(w, 38 + k * 2, 38 + k * 2)) { x = -x; }
            v[k] = x;
        }

        {
            float r = fmaf(v[1], v[2], v[0]);
            size_t at = (size_t)dst_nr * 32u + (size_t)dst_sub * 4u + lane * 4u;

            if (bits(w, 31, 31)) { r = r != r ? 0.0f : r < 0 ? 0.0f : r > 1 ? 1.0f : r; }
            memcpy(&t->grf[at], &r, 4);
            st->lanes++;
        }
    }

    return NULL;
}

/*--------------------------------------------------------------------------
 * The data port, into the run's buffers.
 *------------------------------------------------------------------------*/

static uint32_t surface_bytes_wide(const struct eu_buffer *b)
{
    return b->view == GPU_VIEW_R8 ? b->width * 4u : b->width * 4u;
}

static const char *send(struct thread *t, const uint32_t *w, unsigned exec, const struct eu_run *r,
                        bool *ended)
{
    uint32_t sfid = bits(w, 24, 27), desc = bits(w, 96, 126);
    uint32_t type = (desc >> 14) & 0x1fu, bti = desc & 0xffu, control = (desc >> 8) & 0x3fu;
    uint32_t mlen = (desc >> 25) & 0xfu, rlen = (desc >> 20) & 0x1fu;
    unsigned src = bits(w, 69, 76), dst = bits(w, 53, 60), dst_file = bits(w, 35, 36), lane;
    const struct eu_buffer *b;

    if (sfid == 7) {
        if (!bits(w, 127, 127)) {
            return "a message to the thread spawner that does not end the thread";
        }

        *ended = true;
        return NULL;
    }

    /*
     * The sampler's fetch from a table (`testing.md` 18.705): `ld_lz` (26)
     * with U alone, or `ld` (7) with U, V and LOD, from a buffer seen as
     * R32_UINT. Each lane's word at U into the first channel's register,
     * nought into the second and third, and one into the fourth, as an
     * unsigned format fills what it has not; past the table's end, all
     * nought.
     */
    if (sfid == 2) {
        uint32_t msg = (desc >> 12) & 0x1fu, simd = (desc >> 17) & 3u;
        uint32_t lanes = simd == 2 ? 16u : 8u, regs = lanes / 8u, entries;

        if ((msg != 26 && msg != 7) || (simd != 1 && simd != 2) || ((desc >> 19) & 1u)) {
            return "a sampler message this emulator does not run";
        }

        if (bti >= r->nbuffers) {
            return "a buffer the run does not bind";
        }

        b = &r->buffers[bti];

        if (b->view != GPU_VIEW_TABLE) {
            return "a sampler's fetch from a buffer not bound as a table";
        }

        if (dst_file != FILE_GRF || rlen < 4u * regs || mlen < (msg == 26 ? 1u : 3u) * regs) {
            return "a sampler fetch with the wrong lengths";
        }

        entries = b->pitch * b->height / 4u;

        for (lane = 0; lane < lanes; lane++) {
            uint32_t u, v = 0, one = 1, zero = 0;

            if (!lane_on(t, w, lane, exec)) {
                continue;
            }

            memcpy(&u, &t->grf[(src + lane / 8u) * 32u + (lane % 8u) * 4u], 4);

            if (u < entries) {
                memcpy(&v, &b->bytes[(size_t)u * 4u], 4);
            } else {
                one = 0;
            }

            memcpy(&t->grf[(dst + lane / 8u) * 32u + (lane % 8u) * 4u], &v, 4);
            memcpy(&t->grf[(dst + regs + lane / 8u) * 32u + (lane % 8u) * 4u], &zero, 4);
            memcpy(&t->grf[(dst + 2u * regs + lane / 8u) * 32u + (lane % 8u) * 4u], &zero, 4);
            memcpy(&t->grf[(dst + 3u * regs + lane / 8u) * 32u + (lane % 8u) * 4u], &one, 4);
        }

        return NULL;
    }

    if (sfid != 12) {
        return "a message to a unit this emulator does not run";
    }

    if (bti >= r->nbuffers) {
        return "a buffer the run does not bind";
    }

    b = &r->buffers[bti];

    if (type == 10 || type == 4) {                      /* media block write, read */
        uint32_t hdr[8];
        uint32_t bx, by, bw, bh, j, wide = surface_bytes_wide(b);

        memcpy(hdr, &t->grf[src * 32u], 32);
        bx = hdr[0];
        by = hdr[1];
        bw = (hdr[2] & 0x3ffu) + 1u;
        bh = ((hdr[2] >> 16) & 0x3fu) + 1u;

        if (bh != 1) {
            return "a media block more than a row high";
        }

        if (type == 10) {
            if (mlen < 2 || bw > (mlen - 1u) * 32u) {
                return "a media block write with too little data";
            }

            for (j = 0; j < bw; j++) {
                if ((int32_t)bx >= 0 && bx + j < wide && by < b->height) {
                    b->bytes[(size_t)by * b->pitch + bx + j] = t->grf[(src + 1u) * 32u + j];
                }
            }
        } else {
            if (dst_file != FILE_GRF || rlen * 32u < bw) {
                return "a media block read with too little room";
            }

            for (j = 0; j < bw; j++) {
                t->grf[dst * 32u + j] = ((int32_t)bx >= 0 && bx + j < wide && by < b->height)
                                      ? b->bytes[(size_t)by * b->pitch + bx + j] : 0;
            }
        }

        return NULL;
    }

    if (type == 1 || type == 9) {                       /* untyped surface read, write */
        unsigned simd = ((control >> 4) & 3u) == 1 ? 16u : ((control >> 4) & 3u) == 2 ? 8u : 0u;
        unsigned regs = simd / 8u, k, n = 0;
        size_t size = (size_t)b->pitch * b->height;
        bool chan[4];

        if (simd == 0 || simd < exec) {
            return "an untyped message of a SIMD mode this emulator does not run";
        }

        for (k = 0; k < 4; k++) {
            chan[k] = !((control >> k) & 1u);
            n += chan[k];
        }

        if (mlen != regs * (1u + (type == 9 ? n : 0u)) || (type == 1 && rlen != regs * n)) {
            return "an untyped message whose lengths do not match its channels";
        }

        for (lane = 0; lane < simd; lane++) {
            uint32_t addr;
            unsigned c = 0;

            if (!lane_on(t, w, lane, exec) || !predicate(t, w, lane)) {
                continue;
            }

            memcpy(&addr, &t->grf[src * 32u + lane * 4u], 4);

            for (k = 0; k < 4; k++) {
                size_t at = (size_t)addr + k * 4u;
                size_t reg_at;

                if (!chan[k]) {
                    continue;
                }

                if (type == 9) {
                    reg_at = (src + regs * (1u + c)) * 32u + lane * 4u;

                    if (at + 4 <= size) {
                        memcpy(&b->bytes[at], &t->grf[reg_at], 4);
                    }
                } else if (dst_file == FILE_GRF) {
                    uint32_t v = 0;

                    reg_at = (dst + regs * c) * 32u + lane * 4u;

                    if (at + 4 <= size) {
                        memcpy(&v, &b->bytes[at], 4);
                    }

                    memcpy(&t->grf[reg_at], &v, 4);
                }

                c++;
            }
        }

        return NULL;
    }

    return "a data port message this emulator does not run";
}

/*--------------------------------------------------------------------------
 * A thread, and the grid.
 *------------------------------------------------------------------------*/

/* After an instruction that is not a branch: every lane that was here moves on. */
static void advance(struct thread *t)
{
    unsigned lane;

    for (lane = 0; lane < LANES; lane++) {
        if (t->pcip[lane] == t->ip && !t->broke[lane]) {
            t->pcip[lane] = t->ip + 1u;
        }
    }

    t->ip++;
}

static bool any_at(const struct thread *t, uint32_t ip)
{
    unsigned lane;

    for (lane = 0; lane < t->simd; lane++) {
        if (t->pcip[lane] == ip && !t->broke[lane]) {
            return true;
        }
    }

    return false;
}

/*
 * The structured branches, as the M700's GPU runs them (`testing.md`
 * 18.700). `break` and `continue` both wait at their UIP, which is the
 * loop's `while` - as Mesa aims them from generation 7 - and the `while`
 * lets out the lanes that broke and takes the rest round again where its
 * predicate says. This read UIP as "past the while" for a break, the
 * sixth generation's rule; the hardware kept those lanes off for good.
 */
static const char *structured(struct thread *t, const uint32_t *w, unsigned op, unsigned exec)
{
    int32_t jip = (int32_t)bits(w, 96, 127), uip = (int32_t)bits(w, 64, 95);
    uint32_t to_j, to_u, here = t->ip, next = here + 1u;
    unsigned lane;

    if ((jip & 15) || (uip & 15)) {
        return "a branch target that is not an instruction";
    }

    to_j = (uint32_t)((int32_t)here + jip / 16);
    to_u = (uint32_t)((int32_t)here + uip / 16);

    for (lane = 0; lane < LANES; lane++) {
        /* A lane that broke waits for a while at its UIP, and nothing else
         * wakes it: aimed anywhere else, it is gone for good. */
        bool on = lane < exec && t->pcip[lane] == here && (!t->broke[lane] || op == 0x27), en;

        if (!on) {
            if (lane >= exec && t->pcip[lane] == here) {
                t->pcip[lane] = next;
            }
            continue;
        }

        en = predicate(t, w, lane);

        switch (op) {
        case 0x22: t->pcip[lane] = en ? next : to_j; break;                 /* if */
        case 0x24: t->pcip[lane] = to_j; break;                             /* else */
        case 0x25: t->pcip[lane] = next; break;                             /* endif */
        case 0x27:                                                          /* while */
            t->pcip[lane] = !t->broke[lane] && en ? to_j : next;
            t->broke[lane] = false;
            break;
        case 0x28: case 0x29:                                               /* break, continue */
            t->pcip[lane] = en ? to_u : next;
            t->broke[lane] = en && op == 0x28;
            break;
        }
    }

    switch (op) {
    case 0x22: case 0x24: case 0x28: case 0x29:
        t->ip = any_at(t, next) ? next : to_j;
        break;
    case 0x25:
        t->ip = any_at(t, next) ? next : to_j;
        break;
    case 0x27:
        t->ip = any_at(t, to_j) ? to_j : next;
        break;
    }

    return NULL;
}

static uint64_t *profile;

void eu_profile(uint64_t *per_ip)
{
    profile = per_ip;
}

static const char *thread_run(struct thread *t, const struct eu_run *r, struct eu_stats *st, uint32_t *at)
{
    uint32_t n = r->bytes / 16u;
    uint64_t steps, most = r->most_steps ? r->most_steps : 1000000u;

    for (steps = 0; steps < most; steps++) {
        const uint32_t *w;
        unsigned op, exec;
        const char *why = NULL;
        bool ended = false;

        if (t->ip >= n) {
            *at = t->ip;
            return "a thread ran past the kernel's end";
        }

        w = r->kernel + t->ip * 4u;
        op = bits(w, 0, 6);
        exec = 1u << bits(w, 21, 23);
        *at = t->ip;
        st->instructions++;

        if (profile) {
            profile[t->ip]++;
        }

        if (bits(w, 29, 29)) {
            return "a compacted instruction";
        }

        switch (op) {
        case 0x22: case 0x24: case 0x25: case 0x27: case 0x28: case 0x29:
            why = structured(t, w, op, exec);
            if (why) { return why; }
            continue;
        case 0x20: {                                    /* jmpi: the whole thread, from the next */
            int32_t off = (int32_t)bits(w, 96, 127);
            uint32_t to = (uint32_t)((int32_t)t->ip + 1 + off / 16);
            unsigned lane;

            if (off & 15) {
                return "a jmpi that is not to an instruction";
            }

            if (predicate(t, w, 0)) {
                for (lane = 0; lane < LANES; lane++) {
                    if (t->pcip[lane] == t->ip && !t->broke[lane]) {
                        t->pcip[lane] = to;
                    }
                }

                t->ip = to;
            } else {
                advance(t);
            }

            continue;
        }
        case 0x31:
        case 0x32:
            why = send(t, w, exec, r, &ended);
            break;
        case 0x5b:
            why = three_source(t, w, exec, st);
            break;
        case 0x7e:
            break;
        default:
            why = two_source(t, w, op, exec, st);
            break;
        }

        if (why) {
            return why;
        }

        if (ended) {
            return NULL;
        }

        advance(t);
    }

    return "a thread that did not end";
}

const char *eu_emulate(const struct eu_run *r, struct eu_stats *st, uint32_t *at)
{
    static struct thread t;
    struct eu_stats none;
    uint32_t gx, gy, unused;

    if (st == NULL) { st = &none; }
    if (at == NULL) { at = &unused; }

    memset(st, 0, sizeof *st);

    if ((r->simd != 8 && r->simd != 16) || r->bytes % 16u || r->nconstants > GPU_RUN_CONSTANTS) {
        return "a run the driver would refuse";
    }

    for (gy = 0; gy < r->groups_y; gy++) {
        for (gx = 0; gx < r->groups_x; gx++) {
            const char *why;
            uint32_t r0[8] = { 0 };
            unsigned lane;

            memset(&t, 0, sizeof t);
            t.simd = r->simd;
            r0[1] = gx;                                 /* the thread's group, across */
            r0[6] = gy;                                 /*                    and down */
            memcpy(&t.grf[0], r0, sizeof r0);
            memcpy(&t.grf[32], r->constants, r->nconstants);

            for (lane = 0; lane < LANES; lane++) {
                t.pcip[lane] = lane < r->simd ? 0u : ~0u;
            }

            st->threads++;

            if ((why = thread_run(&t, r, st, at)) != NULL) {
                return why;
            }
        }
    }

    return NULL;
}
