/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Playback Kit's core: frames from the input ring, decoded, converted
 * to the device's format and written a period at a time into the stream's
 * ring (`playback_core.h`, `docs/playback.md`).
 *
 * **Where the samples wait.** A decoded frame goes on the end of `src`; the
 * conversion takes from its front, and keeps back the one sample frame it
 * still interpolates from (`pcmconvert.h`), which stays at the front for the
 * next frame to follow - the same thing `media.feed` did with a Lua string
 * and `:sub`, without the strings. A converted period waits in `period`
 * until it is whole, then goes into the ring's next slot.
 *
 * **Bytes that are not yet a frame** - an MP3's, whose frames cross the
 * window's pieces of the file, and a WAV's, whose sample frames can - wait in
 * `carry`. An AAC record is always one whole frame, the MP4's sample.
 */
#include <string.h>

#include "playback_core.h"
#include "pcmconvert.h"

/* AAC is FFmpeg's, in the image only when FFmpeg is (`FULL`). */
#ifndef PB_HAVE_AAC
#if defined(KOSMOS_FFMPEG)
#define PB_HAVE_AAC 1
#else
#define PB_HAVE_AAC 0
#endif
#endif

#if PB_HAVE_AAC
#include "../ffmpeg/aac_core.h"
#endif

/* ------------------------------------------------------------ the input ring */

int pb_input_init(struct pb_input *in, size_t bytes)
{
    if (bytes < PB_INPUT_DATA + 4096) {
        return 0;
    }

    memset(in, 0, PB_INPUT_DATA);
    in->magic = PB_INPUT_MAGIC;
    in->size = (uint32_t)((bytes - PB_INPUT_DATA) & ~(size_t)7);
    return 1;
}

static uint32_t input_used(const struct pb_input *in)
{
    return in->write - audio_ring_acquire(&in->read);
}

uint32_t pb_input_room(const struct pb_input *in)
{
    uint32_t free_bytes = in->size - input_used(in);
    uint32_t to_end = in->size - (in->write % in->size);
    uint32_t most = free_bytes;

    /* A record never wraps: one that does not fit before the end goes at
     * the front after a skip, so the most that may go now is the larger of
     * what is before the end and what is free at the front. */
    if (to_end < free_bytes) {
        uint32_t front = free_bytes - to_end;

        most = to_end > front ? to_end : front;
    }

    most &= ~7u;
    return most > sizeof(struct pb_record)
           ? most - (uint32_t)sizeof(struct pb_record) : 0;
}

int pb_input_put(struct pb_input *in, const void *bytes, uint32_t n,
                 uint32_t flags)
{
    uint32_t need = (uint32_t)sizeof(struct pb_record) + pb_pad(n);
    uint32_t free_bytes = in->size - input_used(in);
    uint32_t at = in->write;
    uint32_t to_end = in->size - (at % in->size);
    struct pb_record *r;

    if (need > free_bytes) {
        return 0;
    }

    if (need > to_end) {
        /* Not before the end: a skip there, and the record at the front -
         * if the front has room once the skip has taken the rest. */
        if (need > free_bytes - to_end) {
            return 0;
        }

        r = (struct pb_record *)pb_input_at(in, at);
        r->bytes = 0;
        r->flags = PB_SKIP;
        at += to_end;
    }

    r = (struct pb_record *)pb_input_at(in, at);
    r->bytes = n;
    r->flags = flags & ~PB_SKIP;

    if (n > 0) {
        memcpy((uint8_t *)r + sizeof(*r), bytes, n);
    }

    /* The record, skip included, before the index that says it is there. */
    RING_BARRIER();
    in->write = at + need;
    return 1;
}

/*
 * The reader's half: the next record of this generation, or NULL. Skips,
 * and records of another generation - what was written before a seek and
 * not yet read - are let go on the way.
 */
static struct pb_record *input_next(struct pb *p, struct pb_input *in)
{
    for (;;) {
        uint32_t at = in->read;
        struct pb_record *r;

        if (audio_ring_acquire(&in->write) == at) {
            return NULL;
        }

        r = (struct pb_record *)pb_input_at(in, at);

        if (r->flags & PB_SKIP) {
            RING_BARRIER();
            in->read = at + (in->size - (at % in->size));
            continue;
        }

        if (PB_GEN(r->flags) != (p->generation & 0xffffu)) {
            RING_BARRIER();
            in->read = at + (uint32_t)sizeof(*r) + pb_pad(r->bytes);
            continue;
        }

        return r;
    }
}

/* Let a record go: the window may write over it from now. */
static void input_done(struct pb_input *in, const struct pb_record *r)
{
    uint32_t next = in->read + (uint32_t)sizeof(*r) + pb_pad(r->bytes);

    RING_BARRIER();
    in->read = next;
}

/* ------------------------------------------------------------ open, close */

int pb_open(struct pb *p, enum pb_kind kind, long rate, long channels,
            long bits, const uint8_t *config, size_t config_n)
{
    memset(p, 0, sizeof(*p));
    p->kind = kind;

    switch (kind) {
    case PB_WAV:
        if (!pcm_format_ok(rate, channels, bits)) {
            p->why = "a WAV in a format this cannot play";
            return -1;
        }

        p->rate = rate;
        p->channels = channels;
        p->bits = bits;
        return 0;

    case PB_MP3:
        mp3dec_init(&p->mp3);
        p->bits = 16;
        return 0;

    case PB_AAC:
#if PB_HAVE_AAC
        p->aac = aac_open(config, config_n, (unsigned)rate,
                          (unsigned)channels, &p->why);
        p->bits = 16;
        return p->aac != NULL ? 0 : -1;
#else
        (void)config;
        (void)config_n;
        p->why = "this film's sound is AAC, and this system was built "
                 "without its AAC decoder (FULL=0)";
        return -1;
#endif
    }

    p->why = "not a kind of sound this plays";
    return -1;
}

void pb_close(struct pb *p)
{
#if PB_HAVE_AAC
    if (p->aac != NULL) {
        aac_close((struct aac *)p->aac);
    }
#endif
    p->aac = NULL;
}

void pb_restart(struct pb *p, uint32_t generation)
{
    p->generation = generation;
    p->carry_n = 0;
    p->src_bytes = 0;
    p->phase = 0.0;
    p->ended = 0;
    p->period_fill = 0;
    p->heard.finished = 0;

    if (p->kind == PB_MP3) {
        mp3dec_init(&p->mp3);
    }

#if PB_HAVE_AAC
    if (p->aac != NULL) {
        aac_reset((struct aac *)p->aac);
    }
#endif
}

/* ------------------------------------------------------------ decoding */

/* Room for one more decoded frame in `src`, after what waits there. */
static size_t src_room(const struct pb *p)
{
    return PB_SRC_SAMPLES - (p->src_bytes + 1) / 2;
}

/*
 * Bytes of the file into `carry`, for an MP3 or a WAV. False when they do
 * not fit, which the caller avoids by decoding first.
 */
static int carry_add(struct pb *p, const uint8_t *bytes, uint32_t n)
{
    if (n > PB_CARRY - p->carry_n) {
        return 0;
    }

    memcpy(p->carry + p->carry_n, bytes, n);
    p->carry_n += n;
    return 1;
}

static void carry_drop(struct pb *p, uint32_t n)
{
    memmove(p->carry, p->carry + n, p->carry_n - n);
    p->carry_n -= n;
}

/*
 * What waits in `carry` into `src`, as far as it goes. An MP3 a frame at a
 * time, its ID3 tag and stray bytes consumed with no samples, as the MP3
 * Kit's `decode` does; a WAV's whole sample frames as they are.
 */
static void decode_carry(struct pb *p)
{
    if (p->kind == PB_WAV) {
        uint32_t frame = (uint32_t)(p->channels * (p->bits / 8));
        uint32_t whole = p->carry_n - p->carry_n % frame;
        size_t room = src_room(p) * 2;

        if (whole > room) {
            whole = (uint32_t)(room - room % frame);
        }

        /* `src` holds int16; a WAV's bytes go in as they are, and its
         * `bits` say how `pcm_convert` reads them. */
        memcpy((uint8_t *)p->src + p->src_bytes, p->carry, whole);
        p->src_bytes += whole;
        p->heard.frames_in += whole / frame;
        carry_drop(p, whole);
        return;
    }

    while ((p->carry_n >= PB_MP3_AHEAD || (p->ended && p->carry_n > 0))
           && src_room(p) >= MINIMP3_MAX_SAMPLES_PER_FRAME) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&p->mp3, p->carry, (int)p->carry_n,
                                          (int16_t *)((uint8_t *)p->src + p->src_bytes), &info);

        if (info.frame_bytes == 0) {
            /* Not a whole frame yet; more of the file is on its way. At the
             * end there will be none, and what is left is not sound. */
            if (p->ended) {
                p->carry_n = 0;
            }
            return;
        }

        carry_drop(p, (uint32_t)info.frame_bytes);

        if (samples > 0) {
            if (p->rate == 0) {
                p->rate = info.hz;
                p->channels = info.channels;
            }

            p->src_bytes += (size_t)samples * (size_t)info.channels * 2;
            p->heard.frames_in += (uint64_t)samples;
        }
    }
}

/*
 * One record into the core: an AAC frame decoded onto `src`, or a piece of
 * an MP3 or a WAV into `carry`. False when there is no room for it yet, and
 * the record stays where it is for the next pass.
 */
static int take(struct pb *p, const struct pb_record *r)
{
    const uint8_t *bytes = (const uint8_t *)r + sizeof(*r);

    if (p->kind == PB_AAC) {
#if PB_HAVE_AAC
        unsigned samples = 0, channels = 0, rate = 0;

        if (src_room(p) < AAC_ROOM) {
            return 0;
        }

        if (r->bytes > 0
            && aac_decode((struct aac *)p->aac, bytes, r->bytes,
                          (int16_t *)((uint8_t *)p->src + p->src_bytes),
                          src_room(p), 1, &samples,
                          &channels, &rate) == 0) {
            if (p->rate == 0 && samples > 0) {
                p->rate = (long)rate;
                p->channels = (long)channels;
            }

            p->src_bytes += (size_t)samples * channels * 2;
            p->heard.frames_in += samples;
        } else if (r->bytes > 0) {
            /* A damaged frame is passed over, as a damaged picture is: a
             * click in the sound, not the end of it. */
            p->heard.refused++;
            p->why = aac_why((struct aac *)p->aac);
        }
#endif
    } else {
        if (!carry_add(p, bytes, r->bytes)) {
            decode_carry(p);

            if (!carry_add(p, bytes, r->bytes)) {
                return 0;
            }
        }
    }

    if (r->flags & PB_END) {
        p->ended = 1;
    }

    return 1;
}

/* ------------------------------------------------------------ converting */

/*
 * What waits in `src`, converted onto `period` as far as one period goes.
 * Returns true when it made something or consumed something - false when
 * `src` cannot give any more until another frame arrives.
 */
static int convert(struct pb *p, uint32_t period_bytes)
{
    size_t consumed = 0;
    long frames;
    int last = p->ended && p->carry_n == 0;

    if (p->rate == 0 || p->src_bytes == 0) {
        return 0;
    }

    frames = pcm_convert((const unsigned char *)p->src, p->src_bytes,
                         p->rate, p->channels, p->bits, last, &p->phase,
                         p->period + p->period_fill / 2,
                         (long)(period_bytes - p->period_fill), &consumed);

    p->period_fill += (uint32_t)frames * 4u;
    p->heard.frames_out += (uint64_t)frames;

    if (consumed > 0) {
        memmove(p->src, (uint8_t *)p->src + consumed, p->src_bytes - consumed);
        p->src_bytes -= consumed;
    }

    /* At the end, what would not convert - less than one sample frame - is
     * not sound. */
    if (last && frames == 0) {
        p->src_bytes = 0;
    }

    return frames > 0 || consumed > 0;
}

/* The period, whole or the last one padded with silence, into the ring. */
static void write_period(struct pb *p, struct audio_ring *ring)
{
    uint8_t *slot = audio_ring_slot(ring, ring->write);

    memcpy(slot, p->period, p->period_fill);

    if (p->period_fill < ring->period_bytes) {
        memset(slot + p->period_fill, 0, ring->period_bytes - p->period_fill);
    }

    audio_ring_publish(ring, ring->write + 1);
    p->period_fill = 0;
    p->heard.periods++;
}

uint32_t pb_pass(struct pb *p, struct pb_input *in, struct audio_ring *ring)
{
    uint32_t written = 0;
    uint32_t period_bytes = ring->period_bytes;

    if (period_bytes > sizeof(p->period)) {
        period_bytes = (uint32_t)sizeof(p->period);
    }

    while (!p->heard.finished && audio_ring_space(ring) > 0) {
        /* Fill the period from what is already decoded. */
        while (p->period_fill < period_bytes && convert(p, period_bytes)) {
        }

        if (p->period_fill >= period_bytes) {
            write_period(p, ring);
            written++;
            continue;
        }

        /* Then from the bytes waiting in carry, then from the next record. */
        if (p->kind != PB_AAC && p->carry_n > 0) {
            size_t before = p->src_bytes;
            uint32_t carried = p->carry_n;

            decode_carry(p);

            if (p->src_bytes != before || p->carry_n != carried) {
                continue;
            }
        }

        if (!p->ended) {
            struct pb_record *r = input_next(p, in);

            if (r == NULL) {
                p->heard.dry++;
                break;
            }

            if (!take(p, r)) {
                break;                  /* no room yet; nothing lost */
            }

            input_done(in, r);
            continue;
        }

        /* The end: the last of it, padded, and nothing after. */
        if (p->period_fill > 0) {
            write_period(p, ring);
            written++;
        }

        p->heard.finished = 1;
    }

    return written;
}
