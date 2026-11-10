/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `pcmconvert.h`: the resampler `sys.pcm` and the Playback Kit share
 * (`docs/playback.md`, step 1), held on the Mac.
 *
 * **Held to what it was.** `reference` below is `sys.pcm`'s loop as it stood
 * in `sys_user.c` before it moved, its Lua taken out and nothing else
 * changed - the way a vector loop is held to the scalar one. Every format, a
 * spread of rates, pieces of every length and `last` both ways must give the
 * same bytes, the same bytes consumed and the same phase.
 *
 * **And right on its own terms**, which a copy of the old code cannot say:
 * 44.1 kHz stereo comes out as it went in; 22.05 kHz mono doubles each
 * sample to both channels with the midpoint between; 8-bit is centred on
 * 128; the last frame comes out only when `last`; and a file converted in
 * pieces, the phase carried, is the file converted whole - to the sample,
 * or within one step where the rate does not divide 44.1 kHz.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcmconvert.h"

static int failed, checks;

#define CHECK(ok, ...) do { checks++; if (!(ok)) { failed++; \
    printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* sys.pcm's body before the move (0.11.358), its Lua replaced by arguments. */
static long reference(const unsigned char *in, size_t len, long rate,
                      long channels, long bits, double *phase_io,
                      long max_out, int last, int16_t *out, long *used)
{
    long in_frame_bytes = channels * (bits / 8);
    long in_frames, out_frames = 0;
    double step, phase = *phase_io;

    in_frames = (long)(len / (size_t)in_frame_bytes);
    *used = 0;

    if (in_frames < (last ? 1 : 2)) {
        return 0;
    }

    step = (double)rate / (double)44100u;

    while (out_frames * 4 + 4 <= max_out) {
        long at = (long)phase;
        double frac = phase - (double)at;
        int32_t l0, r0, l1, r1;

        if (at + 1 >= in_frames && !(last && at + 1 == in_frames)) {
            break;
        }

        if (bits == 8) {
            const unsigned char *a = in + at * in_frame_bytes;
            const unsigned char *b = (at + 1 < in_frames) ? a + in_frame_bytes : a;

            l0 = ((int32_t)a[0] - 128) << 8;
            r0 = (channels == 2) ? (((int32_t)a[1] - 128) << 8) : l0;
            l1 = ((int32_t)b[0] - 128) << 8;
            r1 = (channels == 2) ? (((int32_t)b[1] - 128) << 8) : l1;
        } else {
            const unsigned char *a = in + at * in_frame_bytes;
            const unsigned char *b = (at + 1 < in_frames) ? a + in_frame_bytes : a;

            l0 = (int16_t)(a[0] | (a[1] << 8));
            r0 = (channels == 2) ? (int16_t)(a[2] | (a[3] << 8)) : l0;
            l1 = (int16_t)(b[0] | (b[1] << 8));
            r1 = (channels == 2) ? (int16_t)(b[2] | (b[3] << 8)) : l1;
        }

        out[out_frames * 2]     = (int16_t)(l0 + (l1 - l0) * frac);
        out[out_frames * 2 + 1] = (int16_t)(r0 + (r1 - r0) * frac);

        out_frames++;
        phase += step;
    }

    {
        long consumed = (long)phase;

        if (consumed > in_frames - (last ? 0 : 1)) {
            consumed = in_frames - (last ? 0 : 1);
        }

        if (consumed < 0) {
            consumed = 0;
        }

        *phase_io = phase - (double)consumed;
        *used = consumed * in_frame_bytes;
    }

    return out_frames;
}

#define ROOM 8192

static unsigned char input[1 << 16];
static int16_t a_out[ROOM / 2], b_out[ROOM / 2];

static void fill(unsigned seed)
{
    for (size_t i = 0; i < sizeof(input); i++) {
        seed = seed * 1103515245u + 12345u;
        input[i] = (unsigned char)(seed >> 16);
    }
}

int main(void)
{
    static const long rates[] = { 8000, 11025, 22050, 32000, 44100, 48000, 96000 };
    static const size_t lens[] = { 0, 1, 2, 3, 4, 7, 8, 100, 1023, 4096, 30001 };
    unsigned differ = 0, cases = 0;

    /* ---- the same as before, everywhere ---- */
    fill(7);

    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++)
    for (long ch = 1; ch <= 2; ch++)
    for (long bits = 8; bits <= 16; bits += 8)
    for (size_t l = 0; l < sizeof(lens) / sizeof(lens[0]); l++)
    for (int last = 0; last <= 1; last++)
    for (long room = 4; room <= ROOM; room *= 8) {
        double pa = 0.37, pb = 0.37;
        long ua;
        size_t ub;
        long na = reference(input, lens[l], rates[r], ch, bits, &pa, room, last, a_out, &ua);
        long nb = pcm_convert(input, lens[l], rates[r], ch, bits, last, &pb, b_out, room, &ub);

        cases++;

        if (na != nb || ua != (long)ub || pa != pb
            || memcmp(a_out, b_out, (size_t)na * 4) != 0) {
            if (differ++ < 5) {
                printf("  differs: %ld Hz, %ld ch, %ld bits, %zu bytes, last %d, room %ld: "
                       "%ld/%ld frames, %ld/%zu used\n",
                       rates[r], ch, bits, lens[l], last, room, na, nb, ua, ub);
            }
        }
    }

    CHECK(differ == 0, "%u of %u cases differ from sys.pcm before the move", differ, cases);

    /* ---- 44.1 kHz stereo: what went in comes out ---- */
    {
        int16_t s[200];
        double ph = 0;
        size_t used;
        long n;

        for (int i = 0; i < 200; i++) s[i] = (int16_t)(i * 300 - 30000);

        n = pcm_convert((const unsigned char *)s, sizeof(s), 44100, 2, 16, 1, &ph,
                        b_out, ROOM, &used);
        CHECK(n == 100 && used == sizeof(s) && memcmp(s, b_out, sizeof(s)) == 0,
              "44.1 kHz stereo did not come out as it went in: %ld frames, %zu used", n, used);
    }

    /* ---- 22.05 kHz mono: each sample to both sides, the midpoint between ---- */
    {
        int16_t s[3] = { 1000, 3000, -1000 };
        double ph = 0;
        size_t used;
        long n = pcm_convert((const unsigned char *)s, sizeof(s), 22050, 1, 16, 1, &ph,
                             b_out, ROOM, &used);

        /* Six: with `last`, the half step past the final frame is that frame. */
        CHECK(n == 6 && b_out[0] == 1000 && b_out[1] == 1000 && b_out[2] == 2000
              && b_out[3] == 2000 && b_out[4] == 3000 && b_out[6] == 1000
              && b_out[8] == -1000,
              "22.05 kHz mono was not doubled with midpoints: %ld frames, %d %d %d %d %d",
              n, b_out[0], b_out[2], b_out[4], b_out[6], b_out[8]);
    }

    /* ---- 8-bit: centred on 128 ---- */
    {
        unsigned char s[4] = { 128, 128, 0, 255 };
        double ph = 0;
        size_t used;
        long n = pcm_convert(s, sizeof(s), 44100, 2, 8, 1, &ph, b_out, ROOM, &used);

        CHECK(n == 2 && b_out[0] == 0 && b_out[1] == 0 && b_out[2] == -32768
              && b_out[3] == 32512,
              "8-bit was not centred on 128: %d %d %d %d", b_out[0], b_out[1], b_out[2], b_out[3]);
    }

    /* ---- the last frame, only when last ---- */
    {
        int16_t s[4] = { 10, 10, 20, 20 };
        double p0 = 0, p1 = 0;
        size_t u0, u1;
        long n0 = pcm_convert((const unsigned char *)s, sizeof(s), 44100, 2, 16, 0, &p0,
                              b_out, ROOM, &u0);
        long n1 = pcm_convert((const unsigned char *)s, sizeof(s), 44100, 2, 16, 1, &p1,
                              a_out, ROOM, &u1);

        CHECK(n0 == 1 && u0 == 4 && n1 == 2 && u1 == 8 && a_out[2] == 20,
              "the last frame: %ld frames and %zu used without last, %ld and %zu with",
              n0, u0, n1, u1);
    }

    /* ---- in pieces, the phase carried, is the whole ---- */
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        static int16_t whole[1 << 16], pieces[1 << 16];
        size_t total = 20000, used;
        double ph = 0;
        long nw = 0, np = 0;
        size_t at = 0;

        fill(11 + (unsigned)r);

        /* Whole, a room at a time, as the caller would. */
        for (;;) {
            long n = pcm_convert(input + at, total - at, rates[r], 2, 16, 1, &ph,
                                 whole + nw * 2, ROOM, &used);
            at += used;
            nw += n;
            if (n == 0 || at >= total) break;
        }

        /* In ragged pieces: what the last call left, then the next 333 bytes. */
        {
            unsigned char carry[ROOM * 2];
            size_t have = 0, fed = 0;

            ph = 0;

            for (;;) {
                size_t more = total - fed < 333 ? total - fed : 333;
                int last = fed + more >= total;
                long n;

                memcpy(carry + have, input + fed, more);
                have += more;
                fed += more;
                n = pcm_convert(carry, have, rates[r], 2, 16, last, &ph,
                                pieces + np * 2, ROOM, &used);
                np += n;
                memmove(carry, carry + used, have - used);
                have -= used;

                if (last && (n == 0 || have < 4)) break;
            }
        }

        /* Within a step: where 44.1 kHz is not a multiple of the rate, the
         * phase reduced at each call rounds differently from the phase run
         * on, and a sample can land one either side - as it always did. */
        {
            int worst = 0;

            for (long i = 0; i < nw * 2 && nw == np; i++) {
                int d = abs(whole[i] - pieces[i]);
                if (d > worst) worst = d;
            }

            CHECK(nw == np && worst <= 1,
                  "%ld Hz in pieces is not the whole: %ld frames against %ld, "
                  "a sample off by %d", rates[r], np, nw, worst);
        }
    }

    if (failed) {
        printf("FAIL: %d of %d checks on pcmconvert.h\n", failed, checks);
        return 1;
    }

    printf("PASS: %d checks on pcmconvert.h (%u cases held to sys.pcm before the move)\n",
           checks, cases);
    return 0;
}
