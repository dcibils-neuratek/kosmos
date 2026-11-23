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

/* ---- pcm_convert_wide (docs/usbaudio.md U10a) ---- */

static void put_sample(unsigned char *at, long bits, int32_t full)
{
    /* `full` is at 2^31; written at `bits`, the low bits dropped. */
    uint32_t v = (uint32_t)full;

    if (bits == 16) { v >>= 16; at[0] = (unsigned char)v; at[1] = (unsigned char)(v >> 8); }
    else if (bits == 24) { v >>= 8; at[0] = (unsigned char)v; at[1] = (unsigned char)(v >> 8);
                           at[2] = (unsigned char)(v >> 16); }
    else { at[0] = (unsigned char)v; at[1] = (unsigned char)(v >> 8);
           at[2] = (unsigned char)(v >> 16); at[3] = (unsigned char)(v >> 24); }
}

/* The error of a resampled sine against the true one, in dB of full scale,
 * over the middle of the output (the filter's edges are silence). */
static double sine_error_db(const int32_t *out, long n, double hz, long out_rate,
                            double amplitude, double delay)
{
    double err = 0, sig = 0;
    long i, from = n / 4, to = 3 * n / 4;

    for (i = from; i < to; i++) {
        double want = amplitude * sin(2 * M_PI * hz * ((double)i - delay) / (double)out_rate);
        double d = (double)out[2 * i] / 2147483648.0 - want;

        err += d * d;
        sig += want * want;
    }

    return 10 * log10(err / sig);
}

static struct pcm_sinc sinc_table;
static unsigned char wide_in[96000 * 8];
static int32_t wide_out[2 * 200000];
static int32_t wide_pieces[2 * 200000];

static void wide_checks(void)
{
    static const long sizes[] = { 16, 24, 32 };
    unsigned k;

    /* The same rate: a copy, each sample itself, widened. */
    for (k = 0; k < 3; k++) {
        long bits = sizes[k], ch, i, n;

        for (ch = 1; ch <= 2; ch++) {
            long fb = ch * bits / 8, frames = 1000, bad = 0;
            double phase = 0;
            size_t used;

            for (i = 0; i < frames * ch; i++) {
                int32_t v = (int32_t)((uint32_t)rand() * 2654435761u);

                put_sample(wide_in + i * (bits / 8), bits, v);
            }

            n = pcm_convert_wide(wide_in, (size_t)(frames * fb), 96000, ch, bits, 0, &phase,
                                 wide_out, 200000, &used, 96000, NULL);

            for (i = 0; i < n; i++) {
                int32_t l = pcm_wide_sample(wide_in + i * fb, bits);
                int32_t r = ch == 2 ? pcm_wide_sample(wide_in + i * fb + bits / 8, bits) : l;

                bad += wide_out[2 * i] != l || wide_out[2 * i + 1] != r;
            }

            CHECK(n == frames && bad == 0 && used == (size_t)(frames * fb),
                  "at the same rate, %ld-bit %s was not copied: %ld frames, %ld differ",
                  bits, ch == 2 ? "stereo" : "mono", n, bad);
        }
    }

    /* 24-bit samples arrive shifted by eight, 16 by sixteen. */
    {
        unsigned char a[4] = { 0x01, 0x02, 0x83, 0 };
        unsigned char b[2] = { 0x34, 0x92 };

        CHECK(pcm_wide_sample(a, 24) == (int32_t)0x83020100u
              && pcm_wide_sample(b, 16) == (int32_t)0x92340000u,
              "24 or 16 bits were not placed at the top of 32: %08x %08x",
              (unsigned)pcm_wide_sample(a, 24), (unsigned)pcm_wide_sample(b, 16));
    }

    /* A 1 kHz sine, 24-bit: through the sinc, and through the linear
     * converter for comparison, 44.1 to 48 and 48 to 96. */
    {
        static const long pairs[][2] = { { 44100, 48000 }, { 48000, 96000 }, { 96000, 48000 } };
        unsigned p;

        for (p = 0; p < 3; p++) {
            long in_rate = pairs[p][0], out_rate = pairs[p][1];
            long frames = in_rate / 2, i, n, ln;
            double phase = 0, amp = 0.5;
            double delay = 0;
            size_t used;
            static int16_t lin[2 * 200000];
            static unsigned char in16[96000 * 4];
            double sinc_db, lin_db;

            for (i = 0; i < frames; i++) {
                double v = amp * sin(2 * M_PI * 1000.0 * (double)i / (double)in_rate);
                int32_t full = (int32_t)(v * 2147483648.0);

                put_sample(wide_in + i * 6, 24, full);
                put_sample(wide_in + i * 6 + 3, 24, full);
                put_sample(in16 + i * 4, 16, full);
                put_sample(in16 + i * 4 + 2, 16, full);
            }

            pcm_sinc_prepare(&sinc_table, in_rate, out_rate);
            n = pcm_convert_wide(wide_in, (size_t)(frames * 6), in_rate, 2, 24, 1, &phase,
                                 wide_out, 200000, &used, out_rate, &sinc_table);
            sinc_db = sine_error_db(wide_out, n, 1000.0, out_rate, amp, delay);

            phase = 0;
            ln = pcm_convert_to(in16, (size_t)(frames * 4), in_rate, 2, 16, 1, &phase, lin,
                                (long)sizeof(lin), &used, out_rate);

            for (i = 0; i < ln; i++) {
                wide_pieces[2 * i] = (int32_t)lin[2 * i] << 16;
            }

            lin_db = sine_error_db(wide_pieces, ln, 1000.0, out_rate, amp, 0);

            /* Under -100 dB, about seventeen bits, and better than the
             * line: by 60 dB going up, and by less coming down, where a
             * 1 kHz tone at 96 kHz is so oversampled a line is nearly right. */
            CHECK(sinc_db < -100.0 && sinc_db < lin_db,
                  "%ld to %ld Hz: the sinc's error is %.1f dB, the linear one's %.1f",
                  in_rate, out_rate, sinc_db, lin_db);
            printf("  1 kHz, %ld to %ld Hz: sinc %.1f dB, linear %.1f dB\n",
                   in_rate, out_rate, sinc_db, lin_db);
        }
    }

    /* In pieces, the phase and the history carried, is the same as whole. */
    {
        long in_rate = 44100, out_rate = 96000, frames = 20000, i, n, whole;
        double phase = 0;
        size_t used, have, taken = 0;
        static unsigned char carry[96000 * 8];
        long got = 0, bad = 0;

        for (i = 0; i < frames * 2; i++) {
            put_sample(wide_in + i * 3, 24, (int32_t)((uint32_t)rand() * 2246822519u));
        }

        pcm_sinc_prepare(&sinc_table, in_rate, out_rate);
        whole = pcm_convert_wide(wide_in, (size_t)(frames * 6), in_rate, 2, 24, 1, &phase,
                                 wide_out, 200000, &used, out_rate, &sinc_table);

        phase = 0;
        have = 0;

        while (taken < (size_t)(frames * 6) || have > 0) {
            size_t piece = (size_t)(1 + rand() % 3000) * 6;
            int last;

            if (piece > (size_t)(frames * 6) - taken) {
                piece = (size_t)(frames * 6) - taken;
            }

            memcpy(carry + have, wide_in + taken, piece);
            have += piece;
            taken += piece;
            last = taken == (size_t)(frames * 6);

            n = pcm_convert_wide(carry, have, in_rate, 2, 24, last, &phase,
                                 wide_pieces + 2 * got, 200000 - got, &used, out_rate, &sinc_table);
            memmove(carry, carry + used, have - used);
            have -= used;
            got += n;

            if (last && n == 0) {
                break;
            }
        }

        /* **Within a 24-bit step, not to the bit.** The phase is a double,
         * carried small in pieces and growing whole, so the fraction between
         * two frames differs in its last places, and so does a tap's weight -
         * as `pcm_convert_to`'s "within one step" above. Below 2^8 at 2^31
         * is below the smallest step a 24-bit device has. */
        {
            long worst = 0;

            for (i = 0; i < whole && i < got; i++) {
                long d0 = labs((long)wide_pieces[2 * i] - (long)wide_out[2 * i]);
                long d1 = labs((long)wide_pieces[2 * i + 1] - (long)wide_out[2 * i + 1]);

                worst = d0 > worst ? d0 : worst;
                worst = d1 > worst ? d1 : worst;
                bad += d0 >= 256 || d1 >= 256;
            }

            CHECK(got == whole && bad == 0,
                  "in pieces it made %ld frames, whole %ld, and %ld differ by a 24-bit step "
                  "or more (the worst by %ld at 2^31)", got, whole, bad, worst);
        }
    }

    /* Narrowing: exact when the low bits are nought, at every size; and
     * when they are not, the right value on average, within a step each
     * time, and with no step taken for a sample already on one. */
    {
        uint32_t seed = 1;
        long bad = 0, i;
        double sum = 0;
        int32_t worst = 0;

        for (i = 0; i < 20000; i++) {
            int32_t v16 = (int32_t)(((uint32_t)rand() & 0xffffu) << 16);
            int32_t v24 = (int32_t)(((uint32_t)rand() & 0xffffffu) << 8);

            bad += pcm_narrow(v16, 16, &seed) != (v16 >> 16);
            bad += pcm_narrow(v24, 24, &seed) != (v24 >> 8);
            bad += pcm_narrow(v24, 32, &seed) != v24;
        }

        CHECK(bad == 0, "a sample with nought below the device's bits was changed: %ld", bad);

        /* 0.3 of a step above 1000: rounds to 1000 or 1001, 1000.3 on
         * average. */
        for (i = 0; i < 100000; i++) {
            int32_t got = pcm_narrow((int32_t)((1000 << 16) + 19661), 16, &seed);
            int32_t d = got - 1000;

            sum += got;
            worst = d > worst ? d : (-d > worst ? -d : worst);
        }

        CHECK(fabs(sum / 100000 - 1000.3) < 0.01 && worst <= 2,
              "dithered to 16 bits, 1000.3 came out as %.4f on average, a step of %d at worst",
              sum / 100000, (int)worst);
        CHECK(pcm_narrow(INT32_MAX, 16, &seed) == 32767 && pcm_narrow(INT32_MIN, 16, &seed) == -32768,
              "narrowing full scale did not stay at the ends");
    }

    /* A constant stays the constant, away from the edges. */
    {
        long frames = 4000, i, n, bad = 0;
        double phase = 0;
        size_t used;
        int32_t dc = 0x30000000;

        for (i = 0; i < frames; i++) {
            put_sample(wide_in + i * 4, 32, dc);
        }

        pcm_sinc_prepare(&sinc_table, 48000, 44100);
        n = pcm_convert_wide(wide_in, (size_t)(frames * 4), 48000, 1, 32, 0, &phase,
                             wide_out, 200000, &used, 44100, &sinc_table);

        for (i = PCM_SINC_HALF; i < n - PCM_SINC_HALF; i++) {
            long d = (long)wide_out[2 * i] - dc;

            /* Each row sums to one in doubles and is kept in floats, whose
             * 24 bits leave it a few parts in 10^8 off: under a 24-bit step. */
            bad += d >= 256 || d <= -256;
        }

        CHECK(n > 3000 && bad == 0, "a constant through the sinc moved by a 24-bit step: "
              "%ld of %ld frames", bad, n);
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

    /*
     * ---- 44.1 to 48 kHz, in the audio server's periods ----
     * `pcm_convert_to`, as the server converts its mix for a USB speaker
     * (`docs/usbaudio.md` U3): a second of a 1 kHz sine, 256 frames at a
     * time with what each call leaves carried, comes out as a second at 48
     * kHz - 48000 frames, less the one held for the next period - and still
     * 1 kHz: a thousand rising crossings.
     */
    {
        static int16_t sine[44100 * 2], out[512 * 2];
        unsigned char carry[512 * 4];
        size_t have = 0, used;
        long frames = 0, crossings = 0, n, k;
        int16_t last = 0;
        double ph = 0;

        for (k = 0; k < 44100; k++) {
            int16_t v = (int16_t)lrint(12000.0 * sin(2.0 * M_PI * 1000.0 * k / 44100.0));
            sine[2 * k] = sine[2 * k + 1] = v;
        }

        for (k = 0; k < 44100; k += 256) {
            long take = 44100 - k < 256 ? 44100 - k : 256;

            memcpy(carry + have, sine + 2 * k, (size_t)take * 4);
            have += (size_t)take * 4;
            n = pcm_convert_to(carry, have, 44100, 2, 16, 0, &ph, out,
                               (long)sizeof(out), &used, 48000);
            memmove(carry, carry + used, have - used);
            have -= used;

            for (long i = 0; i < n; i++) {
                crossings += (last < 0 && out[2 * i] >= 0) ? 1 : 0;
                last = out[2 * i];
            }

            frames += n;
        }

        CHECK(frames >= 47998 && frames <= 48000,
              "a second at 44.1 kHz made %ld frames at 48 kHz, not 48000", frames);
        CHECK(crossings == 999 || crossings == 1000,
              "1 kHz at 48 kHz crossed %ld times in a second, not 1000", crossings);
    }

    wide_checks();

    if (failed) {
        printf("FAIL: %d of %d checks on pcmconvert.h\n", failed, checks);
        return 1;
    }

    printf("PASS: %d checks on pcmconvert.h (%u cases held to sys.pcm before the move)\n",
           checks, cases);
    return 0;
}
