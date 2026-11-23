/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_PCMCONVERT_H
#define KOSMOS_PCMCONVERT_H

/*
 * **Somebody else's samples, in this machine's format** - the one copy.
 *
 * `sys.pcm` was this, inside `sys_user.c`; the Playback Kit needs the same
 * conversion on its thread (`docs/playback.md`, step 1), and two copies of a
 * resampler drift. So it lives here, and `sys.pcm` and the kit both call it.
 * `tools/test_pcmconvert.c` holds it on the Mac.
 *
 * In: 8-bit unsigned or 16-bit signed little-endian, mono or stereo, any
 * rate. Out: 16-bit stereo at `PCM_DEVICE_RATE` (`pcm_convert`), or at any
 * rate (`pcm_convert_to`) - the audio server's mix into a USB speaker's 48
 * kHz (`docs/usbaudio.md` U3), through the same arithmetic rather than a
 * second resampler.
 *
 * **Linear interpolation** between neighbouring frames: not nearest, which
 * is audibly wrong on anything bright, and not a windowed sinc, which is a
 * great deal more arithmetic for a difference a laptop speaker does not
 * show. Saying which matters more than which.
 *
 * **The phase is the caller's**, carried from one piece to the next: a file
 * is converted a piece at a time, the position between input frames does not
 * end on a whole number, and starting each piece at nought puts a click at
 * every boundary.
 *
 * **`last`** says these bytes are the end of the input. Every frame but the
 * final one is interpolated towards the one after it, so without this the
 * final frame waited for a neighbour that never came, and the last sample of
 * every song and film went unplayed (`testing.md` 18.185). With it, the final
 * frame is its own neighbour.
 *
 * Returns the frames written to `out` (two int16 each), at most
 * `max_out_bytes / 4`, and sets `*consumed` to the input bytes finished
 * with - everything before the frame the phase now sits in, which the next
 * piece interpolates from - and `*phase` reduced by as many frames. Nought
 * frames and nought consumed when there is not enough to start (two frames,
 * or one when `last`). The caller has checked the format (`pcm_format_ok`).
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define PCM_DEVICE_RATE 44100u

static inline int pcm_format_ok(long rate, long channels, long bits)
{
    return rate > 0 && (channels == 1 || channels == 2)
           && (bits == 8 || bits == 16);
}

static inline long pcm_convert_to(const unsigned char *in, size_t len,
                                  long rate, long channels, long bits,
                                  int last, double *phase_io, int16_t *out,
                                  long max_out_bytes, size_t *consumed_out,
                                  long out_rate)
{
    long in_frame_bytes = channels * (bits / 8);
    long in_frames = (long)(len / (size_t)in_frame_bytes);
    long out_frames = 0;
    double phase = *phase_io;
    double step;

    *consumed_out = 0;

    if (in_frames < (last ? 1 : 2)) {
        return 0;
    }

    step = (double)rate / (double)(out_rate > 0 ? out_rate : PCM_DEVICE_RATE);

    while (out_frames * 4 + 4 <= max_out_bytes) {
        long at = (long)phase;
        double frac = phase - (double)at;
        int32_t l0, r0, l1, r1;

        /* The last frame has no neighbour to interpolate towards, so it is
         * where this piece stops - unless there is no next piece. */
        if (at + 1 >= in_frames && !(last && at + 1 == in_frames)) {
            break;
        }

        {
            const unsigned char *a = in + at * in_frame_bytes;
            const unsigned char *b = (at + 1 < in_frames)
                                     ? a + in_frame_bytes : a;

            if (bits == 8) {
                /* Unsigned, centred on 128, which is what an 8-bit WAV is. */
                l0 = ((int32_t)a[0] - 128) << 8;
                r0 = (channels == 2) ? (((int32_t)a[1] - 128) << 8) : l0;
                l1 = ((int32_t)b[0] - 128) << 8;
                r1 = (channels == 2) ? (((int32_t)b[1] - 128) << 8) : l1;
            } else {
                /* A byte at a time: nothing promises two-byte alignment. */
                l0 = (int16_t)(a[0] | (a[1] << 8));
                r0 = (channels == 2) ? (int16_t)(a[2] | (a[3] << 8)) : l0;
                l1 = (int16_t)(b[0] | (b[1] << 8));
                r1 = (channels == 2) ? (int16_t)(b[2] | (b[3] << 8)) : l1;
            }
        }

        out[out_frames * 2]     = (int16_t)(l0 + (l1 - l0) * frac);
        out[out_frames * 2 + 1] = (int16_t)(r0 + (r1 - r0) * frac);

        out_frames++;
        phase += step;
    }

    {
        long consumed = (long)phase;

        /* All of it at the end; otherwise the frame the phase sits in stays
         * for the next piece to interpolate from. */
        if (consumed > in_frames - (last ? 0 : 1)) {
            consumed = in_frames - (last ? 0 : 1);
        }

        if (consumed < 0) {
            consumed = 0;
        }

        *phase_io = phase - (double)consumed;
        *consumed_out = (size_t)(consumed * in_frame_bytes);
    }

    return out_frames;
}

static inline long pcm_convert(const unsigned char *in, size_t len,
                               long rate, long channels, long bits, int last,
                               double *phase_io, int16_t *out,
                               long max_out_bytes, size_t *consumed_out)
{
    return pcm_convert_to(in, len, rate, channels, bits, last, phase_io, out,
                          max_out_bytes, consumed_out, PCM_DEVICE_RATE);
}

/*
 * **Past sixteen bits, and a filter rather than a line** (`docs/usbaudio.md`
 * U10a; `docs/player.md` decision 5).
 *
 * In: 16, 24 or 32-bit signed little-endian integer, mono or stereo, any
 * rate. Out: 32-bit stereo frames - two `int32_t` each, full scale at 2^31,
 * so 16 bits arrive shifted by sixteen and 24 by eight - at `out_rate`.
 *
 * **At the same rate it copies.** No interpolation and no filter: each
 * sample comes out as itself, widened, which is what lets a 96/24 file
 * reach a 96/24 device bit for bit. The phase stays at nought and every
 * frame is consumed.
 *
 * **Across rates, a windowed sinc**: `PCM_SINC_HALF` taps either side of
 * the point between two input frames, a Blackman window, its cutoff at the
 * lower of the two rates' Nyquist and a little under it, and each row of
 * the table summed to one so a constant stays the constant. The table is
 * `PCM_SINC_PHASES` rows of the fraction between two frames, interpolated
 * between rows; `pcm_sinc_prepare` makes it once for a pair of rates, with
 * `sin` and `cos`, and it is the caller's to keep (66 KB). Thirty-two taps a
 * side, because sixteen left 96 to 48 kHz at -90 dB: halving the rate halves
 * the cutoff, and the same taps then span half as many of the sinc's lobes. The linear
 * `pcm_convert_to` above stays for `sys.pcm` and the Playback Kit until they
 * ask for this.
 *
 * **In integers, not floats** (`testing.md` 18.1006). The coefficients are
 * Q30 - one is 2^30 - each row rounded to sum to exactly that, and a frame
 * is 64 taps of a 32-bit sample by a 32-bit coefficient summed in 64 bits
 * and rounded once. The sum cannot overflow: a row's coefficients add up,
 * in magnitude, to at most 2.24 (measured for every pair of rates from 8 to
 * 192 kHz), so it stays under 2^31 x 2.24 x 2^30 = 2^62.2. The same
 * -117 dB as in floats, and a second of 44.1 kHz stereo to 48 kHz in 4.8 ms
 * on this Mac's ARM cores against 8.5 (2.6 against 2.1 through Rosetta).
 *
 * **The phase is the caller's, as above, and so is the history**: a frame is
 * kept, not consumed, while a later output frame still needs it as a tap -
 * `*consumed` stops `PCM_SINC_HALF - 1` frames behind the phase. Before the
 * first frame the input is taken as silence; after the last, when `last`, as
 * well.
 *
 * Returns the frames written to `out` (at most `max_out_frames`), and sets
 * `*consumed` to the input bytes finished with and `*phase` reduced by as
 * many frames. The caller has checked the format (`pcm_wide_format_ok`).
 */

#define PCM_SINC_HALF    32
#define PCM_SINC_PHASES 256

struct pcm_sinc {
    long  in_rate, out_rate;
    int32_t k[PCM_SINC_PHASES + 1][2 * PCM_SINC_HALF];      /* Q30 */
};

static inline int pcm_wide_format_ok(long rate, long channels, long bits)
{
    return rate > 0 && (channels == 1 || channels == 2)
           && (bits == 16 || bits == 24 || bits == 32);
}

static inline void pcm_sinc_prepare(struct pcm_sinc *s, long in_rate, long out_rate)
{
    const double pi = 3.14159265358979323846;
    /* The pass band ends a little under the lower Nyquist, so the window
     * has room to fall before it: 0.9 of it, which leaves 20 kHz of a
     * 44.1 kHz file whole and costs nothing anybody hears. */
    double cutoff = (out_rate < in_rate ? (double)out_rate / (double)in_rate : 1.0) * 0.9;
    int p, j;

    s->in_rate = in_rate;
    s->out_rate = out_rate;

    for (p = 0; p <= PCM_SINC_PHASES; p++) {
        double frac = (double)p / PCM_SINC_PHASES;
        double sum = 0, v_of[2 * PCM_SINC_HALF];
        int64_t total = 0;
        int most = 0;

        for (j = 0; j < 2 * PCM_SINC_HALF; j++) {
            /* Tap j is input frame at - (HALF - 1) + j; the point is at + frac. */
            double d = (double)(j - (PCM_SINC_HALF - 1)) - frac;
            double x = d / PCM_SINC_HALF;
            double w = (x <= -1.0 || x >= 1.0) ? 0.0
                       : 0.42 + 0.5 * cos(pi * x) + 0.08 * cos(2.0 * pi * x);
            double a = cutoff * d * pi;
            double v = (d == 0.0 ? cutoff : sin(a) / (d * pi)) * w;

            v_of[j] = v;
            sum += v;
        }

        for (j = 0; j < 2 * PCM_SINC_HALF; j++) {
            double q = v_of[j] / sum * 1073741824.0;

            s->k[p][j] = (int32_t)(q < 0 ? q - 0.5 : q + 0.5);
            total += s->k[p][j];

            if (s->k[p][j] > s->k[p][most]) {
                most = j;
            }
        }

        /* What rounding left over, on the largest tap, so a constant comes
         * out as exactly itself. */
        s->k[p][most] += (int32_t)(((int64_t)1 << 30) - total);
    }
}

static inline int32_t pcm_wide_sample(const unsigned char *a, long bits)
{
    if (bits == 16) {
        return (int32_t)((uint32_t)(uint16_t)(a[0] | (a[1] << 8)) << 16);
    }

    if (bits == 24) {
        return (int32_t)(((uint32_t)a[0] << 8) | ((uint32_t)a[1] << 16)
                         | ((uint32_t)a[2] << 24));
    }

    return (int32_t)((uint32_t)a[0] | ((uint32_t)a[1] << 8)
                     | ((uint32_t)a[2] << 16) | ((uint32_t)a[3] << 24));
}

static inline long pcm_convert_wide(const unsigned char *in, size_t len,
                                    long rate, long channels, long bits,
                                    int last, double *phase_io, int32_t *out,
                                    long max_out_frames, size_t *consumed_out,
                                    long out_rate, const struct pcm_sinc *sinc)
{
    long fb = channels * (bits / 8);
    long in_frames = (long)(len / (size_t)fb);
    long n = 0;
    double phase = *phase_io;
    double step;

    *consumed_out = 0;

    if (rate == out_rate) {
        for (n = 0; n < in_frames && n < max_out_frames; n++) {
            const unsigned char *a = in + n * fb;
            int32_t l = pcm_wide_sample(a, bits);

            out[2 * n] = l;
            out[2 * n + 1] = (channels == 2) ? pcm_wide_sample(a + bits / 8, bits) : l;
        }

        *phase_io = 0;
        *consumed_out = (size_t)(n * fb);
        return n;
    }

    step = (double)rate / (double)out_rate;

    while (n < max_out_frames) {
        long at = (long)phase;
        double where = (phase - (double)at) * PCM_SINC_PHASES;
        int row = (int)where;
        /* The fraction between two rows in 24 bits: with 16, a phase
         * carried across two calls could step it by one, and a full-scale
         * sample moved by a few hundred at 2^31. A row's difference is
         * under 2^23, so its product with this stays under 2^47. */
        int64_t t = (int64_t)((where - row) * 16777216.0);
        int64_t l = 0, r = 0;
        int j;

        /* Every tap there, or the end of the input when it is the end. */
        if (last ? at >= in_frames : at + PCM_SINC_HALF >= in_frames) {
            break;
        }

        for (j = 0; j < 2 * PCM_SINC_HALF; j++) {
            long i = at - (PCM_SINC_HALF - 1) + j;
            int64_t k = sinc->k[row][j]
                        + ((((int64_t)sinc->k[row + 1][j] - sinc->k[row][j]) * t) >> 24);
            int32_t a, b;

            if (i < 0 || i >= in_frames) {
                continue;                   /* silence either side */
            }

            a = pcm_wide_sample(in + i * fb, bits);
            b = (channels == 2) ? pcm_wide_sample(in + i * fb + bits / 8, bits) : a;
            l += (int64_t)a * k;
            r += (int64_t)b * k;
        }

        l = (l + ((int64_t)1 << 29)) >> 30;                   /* rounded once */
        r = (r + ((int64_t)1 << 29)) >> 30;
        l = l > 2147483647 ? 2147483647 : (l < -2147483647 - 1 ? -2147483647 - 1 : l);
        r = r > 2147483647 ? 2147483647 : (r < -2147483647 - 1 ? -2147483647 - 1 : r);
        out[2 * n] = (int32_t)l;
        out[2 * n + 1] = (int32_t)r;
        n++;
        phase += step;
    }

    {
        long keep_from = (long)phase - (PCM_SINC_HALF - 1);

        if (keep_from > in_frames) {
            keep_from = in_frames;
        }

        if (keep_from < 0) {
            keep_from = 0;
        }

        *phase_io = phase - (double)keep_from;
        *consumed_out = (size_t)(keep_from * fb);
    }

    return n;
}

/*
 * **Narrowing, exact when it can be and dithered when not**
 * (`docs/usbaudio.md` U10, point 4). `v` is at full scale 2^31; `bits`, 16
 * to 32, is what the device keeps. A sample whose bits below the device's
 * are nought is cut to them and is exactly what came in - every 16-bit
 * stream at unity. One that has more is given triangular dither of one step
 * and rounded, so what it had below the step is heard as a little noise
 * rather than as distortion. `*seed` is the caller's, a xorshift's state.
 */
static inline uint32_t pcm_dither_next(uint32_t *seed)
{
    uint32_t x = *seed;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return x;
}

static inline int32_t pcm_narrow(int32_t v, unsigned bits, uint32_t *seed)
{
    unsigned drop = 32u - bits;
    int64_t step, x, top;

    if (drop == 0 || ((uint32_t)v & ((1u << drop) - 1u)) == 0) {
        return v >> drop;
    }

    step = (int64_t)1 << drop;
    x = (int64_t)v + (int64_t)(pcm_dither_next(seed) & (uint32_t)(step - 1))
        + (int64_t)(pcm_dither_next(seed) & (uint32_t)(step - 1)) - step + step / 2;
    x >>= drop;
    top = ((int64_t)1 << (bits - 1)) - 1;

    if (x > top)      { x = top; }
    if (x < -top - 1) { x = -top - 1; }

    return (int32_t)x;
}

#endif
