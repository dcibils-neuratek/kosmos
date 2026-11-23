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
 * `carry`.
 *
 * **AAC went in 0.12.97** (`docs/media.md` M6d): a film's sound is decoded
 * by the Media Kit in `media.elf` and arrives here as a WAV's PCM, so the
 * old FFmpeg closure it was decoded by is gone.
 */
#include <string.h>

#include "audioproto.h"
#include "playback_core.h"
#include "pcmconvert.h"

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
            long bits)
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

    case PB_PCM:
        return 0;                       /* each song says its format */
    }

    p->why = "not a kind of sound this plays";
    return -1;
}

void pb_close(struct pb *p)
{
    (void)p;                            /* nothing a core holds outside itself */
}

void pb_restart(struct pb *p, uint32_t generation)
{
    p->generation = generation;
    p->carry_n = 0;
    p->src_bytes = 0;
    p->phase = 0.0;
    p->ended = 0;
    p->fed = 0;
    p->period_fill = 0;
    p->heard.finished = 0;

    /* PB_PCM: no song until its record comes, and a ring to be named. */
    p->have_song = 0;
    p->rec_at = 0;
    p->slot_fill = 0;
    p->out_rate = p->out_bits = p->out_channels = 0;
    p->heard.change = 0;

    if (p->kind == PB_MP3) {
        mp3dec_init(&p->mp3);
    }
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
 * One record into the core: a piece of an MP3 or a WAV into `carry`. False when there is no room for it yet, and
 * the record stays where it is for the next pass.
 */
static int take(struct pb *p, const struct pb_record *r)
{
    const uint8_t *bytes = (const uint8_t *)r + sizeof(*r);

    if (!carry_add(p, bytes, r->bytes)) {
        decode_carry(p);

        if (!carry_add(p, bytes, r->bytes)) {
            return 0;
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

/* ------------------------------------------------------------ decoded PCM */

int pb_ring_format(struct pb *p, uint32_t rate, uint32_t bits, uint32_t channels,
                   uint32_t period_bytes)
{
    if (!audio_format_valid(rate, bits, channels)
        || period_bytes == 0 || period_bytes % (channels * (bits / 8u)) != 0) {
        p->why = "a ring in a format a stream cannot have";
        return 0;
    }

    p->out_rate = rate;
    p->out_bits = bits;
    p->out_channels = channels;
    p->slot_fill = 0;
    p->heard.ring++;
    p->heard.ring_frames = 0;
    p->heard.change = 0;
    p->heard.finished = 0;
    return 1;
}

static int song_valid(const struct pb_song *s)
{
    uint32_t bits = pb_stream_bits(s->format, s->bits);

    return bits != 0 && audio_format_valid(s->rate, bits, s->channels)
           && (s->format != PB_PCM_S16 || s->bits == 0 || s->bits <= 16)
           && s->bits <= 32;
}

/* A song's record taken: what is heard of it, and whether the ring suits it. */
static void song_begin(struct pb *p, const struct pb_record *r)
{
    struct pb_song s;

    if (r->bytes != sizeof s) {
        p->have_song = 0;
        p->heard.refused++;
        p->why = "a song's record of the wrong size";
        return;
    }

    memcpy(&s, (const uint8_t *)r + sizeof(*r), sizeof s);

    if (!song_valid(&s)) {
        p->have_song = 0;
        p->heard.refused++;
        p->why = "a song in a format this cannot play";
        return;
    }

    p->song = s;
    p->have_song = 1;
    p->index = s.first;
    p->keep_from = s.delay + s.from;
    p->keep_to = s.length != 0 ? s.delay + s.length : UINT64_MAX;
    p->length_checked = s.length == 0;
    p->marked = 0;
}

/*
 * **Padding or a wrong length?** At the song's stated end, the frames still
 * to come of it - up to the next song's record or the end - are counted
 * without being taken. An encoder pads less than `PB_PAD_MOST`; more than
 * that is a length that was wrong, and the song is played to its end.
 * 1 settled, 0 not yet: the feeder has not written as far as the song's
 * end, and the pass waits for it. Bounded by what the ring holds, and done
 * once a song.
 */
static int padding_settled(struct pb *p, struct pb_input *in)
{
    uint32_t fb = p->song.channels * pb_sample_bytes(p->song.format);
    uint32_t at = in->read;
    uint32_t end = audio_ring_acquire(&in->write);
    uint64_t rest = 0;
    int head = 1;

    while (at != end) {
        const struct pb_record *r = (const struct pb_record *)pb_input_at(in, at);

        if (r->flags & PB_SKIP) {
            at += in->size - (at % in->size);
            continue;
        }

        if (PB_GEN(r->flags) == (p->generation & 0xffffu)) {
            if (r->flags & (PB_SONG | PB_END)) {
                if (r->flags & PB_END) {
                    rest += r->bytes / fb;
                }
                break;
            }

            rest += (r->bytes - (head ? p->rec_at : 0)) / fb;

            if (rest > PB_PAD_MOST) {
                break;
            }
        }

        head = 0;
        at += (uint32_t)sizeof(*r) + pb_pad(r->bytes);
    }

    if (rest > PB_PAD_MOST) {
        p->keep_to = UINT64_MAX;
        p->heard.untrusted++;
        p->why = "a song's stated length is far short of its sound; played to its end";
    } else if (at == end) {
        return 0;
    }

    p->length_checked = 1;
    return 1;
}

static void put_le(uint8_t *at, uint32_t v, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++) {
        at[i] = (uint8_t)(v >> (8u * i));
    }
}

static int32_t float_sample(float f)
{
    double x = (double)f * 2147483648.0;

    if (x >= 2147483647.0) {
        return 2147483647;
    }

    if (x <= -2147483648.0) {
        return -2147483647 - 1;
    }

    return (int32_t)(x < 0 ? x - 0.5 : x + 0.5);
}

/*
 * `n` frames of the song into the ring's slot at `slot`: the container of a
 * sample changed, never its value - s16 as itself, s32 to its top 24 bits
 * when the stream is 24-bit (a 24-bit source has noughts below them), f32
 * rounded to 32-bit integers. A copy of a few kilobytes a millisecond.
 */
static void put_frames(const struct pb *p, uint8_t *slot, const uint8_t *src, uint32_t n)
{
    uint32_t samples = n * p->song.channels;
    uint32_t ob = p->out_bits / 8u;

    switch (p->song.format) {
    case PB_PCM_S16:
        memcpy(slot, src, (size_t)samples * 2u);
        return;

    case PB_PCM_S32:
        if (ob == 4) {
            memcpy(slot, src, (size_t)samples * 4u);
            return;
        }

        for (uint32_t i = 0; i < samples; i++) {
            int32_t v;

            memcpy(&v, src + 4u * i, 4);
            put_le(slot + ob * i, (uint32_t)v >> 8, 3);
        }
        return;

    case PB_PCM_F32:
        for (uint32_t i = 0; i < samples; i++) {
            float f;

            memcpy(&f, src + 4u * i, 4);
            put_le(slot + 4u * i, (uint32_t)float_sample(f), 4);
        }
        return;
    }
}

/* The slot being filled, the rest of it silence, published. */
static void pcm_publish(struct pb *p, struct audio_ring *ring)
{
    uint8_t *slot = audio_ring_slot(ring, ring->write);
    uint32_t fb = p->out_channels * (p->out_bits / 8u);

    if (p->slot_fill < ring->period_bytes) {
        memset(slot + p->slot_fill, 0, ring->period_bytes - p->slot_fill);
    }

    audio_ring_publish(ring, ring->write + 1);
    p->heard.ring_frames += ring->period_bytes / fb;
    p->heard.frames_out += p->slot_fill / fb;
    p->heard.periods++;
    p->slot_fill = 0;
}

static void mark(struct pb *p)
{
    struct pb_mark *m = &p->heard.mark[p->heard.marks % PB_MARKS];
    uint32_t fb = p->out_channels * (p->out_bits / 8u);

    m->song = p->song.song;
    m->ring = p->heard.ring;
    m->at = p->heard.ring_frames + p->slot_fill / fb;
    m->from = p->index - p->song.delay;
    p->heard.marks++;
    p->marked = 1;
}

/*
 * **The decoded path's pass**: records straight into the ring's slot, with
 * no carry and no conversion of rate - a record is taken a part at a time,
 * and let go once all of it is in. The trimming is two comparisons of the
 * decoded-stream index, so a join of two songs is the first's last kept
 * frame and the second's first kept frame, in the same period.
 */
static uint32_t pcm_pass(struct pb *p, struct pb_input *in, struct audio_ring *ring)
{
    uint32_t written = 0;
    uint32_t period = ring->period_bytes;

    if (p->out_bits == 0) {
        return 0;                       /* no ring format yet: `pb_ring_format` */
    }

    while (!p->heard.finished && !p->heard.change) {
        uint32_t ofb = p->out_channels * (p->out_bits / 8u);
        struct pb_record *r;

        if (p->slot_fill >= period) {
            pcm_publish(p, ring);
            written++;
            continue;
        }

        if (audio_ring_space(ring) == 0) {
            break;
        }

        r = input_next(p, in);

        if (r == NULL) {
            if (p->fed) {
                p->heard.dry++;
            }
            break;
        }

        p->fed = 1;

        if (r->flags & PB_SONG) {
            song_begin(p, r);
            input_done(in, r);
            p->rec_at = 0;

            if (p->have_song
                && (p->song.rate != p->out_rate || p->song.channels != p->out_channels
                    || pb_stream_bits(p->song.format, p->song.bits) != p->out_bits)) {
                /* Another format: what this ring has is written, and the
                 * next one is the caller's to open. */
                if (p->slot_fill > 0) {
                    pcm_publish(p, ring);
                    written++;
                }

                p->heard.change = 1;
                p->heard.change_rate = p->song.rate;
                p->heard.change_bits = pb_stream_bits(p->song.format, p->song.bits);
                p->heard.change_channels = p->song.channels;
            }
            continue;
        }

        if (r->flags & PB_END) {
            input_done(in, r);
            p->rec_at = 0;

            if (p->slot_fill > 0) {
                pcm_publish(p, ring);
                written++;
            }

            p->heard.finished = 1;
            break;
        }

        if (!p->have_song) {
            p->heard.refused++;         /* samples of no song: not heard */
            input_done(in, r);
            p->rec_at = 0;
            continue;
        }

        {
            uint32_t fb = p->song.channels * pb_sample_bytes(p->song.format);
            uint64_t avail = (r->bytes - p->rec_at) / fb;
            uint64_t n;
            const uint8_t *src = (const uint8_t *)r + sizeof(*r) + p->rec_at;

            if (avail == 0) {
                input_done(in, r);      /* a part frame at its end is not sound */
                p->rec_at = 0;
                continue;
            }

            if (p->index < p->keep_from) {
                n = p->keep_from - p->index;
                n = n < avail ? n : avail;
            } else if (p->index >= p->keep_to) {
                if (!p->length_checked && !padding_settled(p, in)) {
                    break;              /* the song's end not yet written */
                }

                if (p->index < p->keep_to) {
                    continue;           /* the length was not believed */
                }

                n = avail;
            } else {
                uint64_t room = (period - p->slot_fill) / ofb;
                uint64_t left = p->keep_to - p->index;

                n = avail < room ? avail : room;
                n = n < left ? n : left;

                if (!p->marked) {
                    mark(p);
                }

                put_frames(p, audio_ring_slot(ring, ring->write) + p->slot_fill, src,
                           (uint32_t)n);
                p->slot_fill += (uint32_t)n * ofb;
            }

            p->heard.frames_in += n;
            p->index += n;
            p->rec_at += (uint32_t)(n * fb);

            if (p->rec_at + fb > r->bytes) {
                input_done(in, r);
                p->rec_at = 0;
            }
        }
    }

    return written;
}

/* ------------------------------------------------------------ a pass */

/* A WAV's or an MP3's pass: bytes of the file, decoded and converted. */
static uint32_t coded_pass(struct pb *p, struct pb_input *in, struct audio_ring *ring)
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
        if (p->carry_n > 0) {
            size_t before = p->src_bytes;
            uint32_t carried = p->carry_n;

            decode_carry(p);

            if (p->src_bytes != before || p->carry_n != carried) {
                continue;
            }
        }

        if (!p->ended) {
            struct pb_record *r = input_next(p, in);

            /*
             * Dry only once this start has had a frame: before the first,
             * the window has not begun - a seek's first pass comes before
             * its first frame - and nothing playing has been starved.
             */
            if (r == NULL) {
                if (p->fed) {
                    p->heard.dry++;
                }
                break;
            }

            if (!take(p, r)) {
                break;                  /* no room yet; nothing lost */
            }

            input_done(in, r);
            p->fed = 1;
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

uint32_t pb_pass(struct pb *p, struct pb_input *in, struct audio_ring *ring)
{
    return p->kind == PB_PCM ? pcm_pass(p, in, ring) : coded_pass(p, in, ring);
}
