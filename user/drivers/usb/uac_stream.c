/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A USB speaker's stream, as arithmetic: `uac_stream.h` says what and why.
 */

#include "uac_stream.h"

#include <string.h>

#define UFRAMES_A_SECOND    8000u

void uac_clock_init(struct uac_clock *k, uint32_t rate, uint32_t uframes)
{
    k->rate = rate;
    k->uframes = uframes == 0 ? 1u : uframes;
    k->remainder = 0;
}

uint32_t uac_clock_next(struct uac_clock *k)
{
    uint64_t owed = (uint64_t)k->rate * k->uframes + k->remainder;
    uint32_t frames = (uint32_t)(owed / UFRAMES_A_SECOND);

    k->remainder = (uint32_t)(owed % UFRAMES_A_SECOND);
    return frames;
}

uint32_t uac_clock_most(uint32_t rate, uint32_t uframes)
{
    uint64_t owed = (uint64_t)rate * (uframes == 0 ? 1u : uframes);

    return (uint32_t)((owed + UFRAMES_A_SECOND - 1u) / UFRAMES_A_SECOND);
}

void uac_tone_init(struct uac_tone *t, uint32_t hz, uint32_t rate,
                   int32_t level)
{
    t->phase = 0;
    t->step = rate == 0 ? 0 : (uint32_t)(((uint64_t)hz << 32) / rate);
    t->level = level < 0 ? 0 : level > 32767 ? 32767 : level;
}

/*
 * Bhaskara I's sine over half a turn: with x = pi t, t in [0, 1],
 * sin x ~ 16 t (1 - t) / (5 - 4 t (1 - t)). Here t is m / 32768 of the
 * half turn, so p = m (32768 - m) is t (1 - t) in 2^30ths.
 */
int32_t uac_tone_next(struct uac_tone *t)
{
    uint32_t half = t->phase >> 31;
    uint32_t m = (t->phase >> 16) & 0x7FFFu;
    int64_t p = (int64_t)m * (int64_t)(32768u - m);
    int64_t value = 16 * p * t->level / (5 * ((int64_t)1 << 30) - 4 * p);

    t->phase += t->step;
    return (int32_t)(half ? -value : value);
}

uint32_t uac_tone_fill(struct uac_tone *t, uint8_t *out, uint32_t frames,
                       unsigned channels, unsigned subframe)
{
    uint32_t bytes = frames * channels * subframe, f;
    unsigned ch, b;

    if (subframe < 2 || subframe > 4) {
        memset(out, 0, bytes);
        return bytes;
    }

    for (f = 0; f < frames; f++) {
        /* A 16-bit sample, widened into the subframe's high bytes. */
        uint32_t sample = (uint32_t)uac_tone_next(t) << ((subframe - 2u) * 8u);

        for (ch = 0; ch < channels; ch++) {
            for (b = 0; b < subframe; b++) {
                *out++ = (uint8_t)(sample >> (8u * b));
            }
        }
    }

    return bytes;
}
