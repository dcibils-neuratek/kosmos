/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_PLAYBACK_CORE_H
#define KOSMOS_PLAYBACK_CORE_H

/*
 * **The Playback Kit's core** (`docs/playback.md`): compressed frames in,
 * periods of the device's sound out, into a stream's `audio_ring`. Nothing
 * here knows about threads, Lua or Kosmos, so the Mac holds it
 * (`tools/test_playback.c`); `playback_kosmos.c` puts it on a thread in the
 * audio band and gives it a Lua face.
 *
 * **Nothing here allocates after `pb_open`.** The pass runs on the audio
 * thread, and the audio thread never touches the heap - the Synth Kit's
 * rule, for the same reason.
 */

#include <stddef.h>
#include <stdint.h>

#include "audioring.h"
#include "minimp3.h"                   /* mp3dec_t: declarations only */

/* ------------------------------------------------------------ the input ring */

/*
 * **Compressed frames, the window to the thread**: a region of one writer -
 * the window, which reads the file - and one reader, the thread. The indices
 * are byte offsets that only rise; `write - read` is what is waiting, across
 * the wrap of 2^32 as `audio_ring`'s are.
 *
 * A record is `struct pb_record` and its bytes, padded to eight, and never
 * split by the end of the ring: one that would not fit before it is
 * preceded by a `PB_SKIP` record to the end, and starts at the front.
 */
#define PB_INPUT_MAGIC 0x4b425950u          /* 'PYBK' */
#define PB_INPUT_DATA  64u                  /* records start here */

struct pb_input {
    uint32_t magic;
    uint32_t size;                          /* bytes of records, a multiple of 8 */
    uint32_t reserved[2];
    volatile uint32_t write;                /* the window's */
    uint32_t pad0[7];                       /* a cache line apart */
    volatile uint32_t read;                 /* the thread's */
};

_Static_assert(sizeof(struct pb_input) <= PB_INPUT_DATA,
               "the input ring's header has grown into its records");

struct pb_record {
    uint32_t bytes;                         /* what follows, unpadded */
    uint32_t flags;                         /* PB_* | generation << 16 */
};

#define PB_SKIP      0x1u                   /* to the end of the ring; no bytes */
#define PB_END       0x2u                   /* no frame will come after this one */
#define PB_GEN(f)    ((f) >> 16)

static inline uint32_t pb_pad(uint32_t n)
{
    return (n + 7u) & ~7u;
}

static inline uint8_t *pb_input_at(struct pb_input *in, uint32_t offset)
{
    return (uint8_t *)in + PB_INPUT_DATA + (offset % in->size);
}

/* Lay out an input ring over `bytes` of memory; false if too small. */
int pb_input_init(struct pb_input *in, size_t bytes);

/*
 * The writer's half. How many bytes one frame may be and still go now, and
 * a frame put - false when it does not fit yet, which is the window being
 * ahead; it comes back next tick.
 */
uint32_t pb_input_room(const struct pb_input *in);
int      pb_input_put(struct pb_input *in, const void *bytes, uint32_t n,
                      uint32_t flags);

/* ------------------------------------------------------------ the core */

enum pb_kind { PB_WAV = 1, PB_MP3 = 2, PB_AAC = 3 };

/*
 * How much of a source's sound is held between frames: a decoded frame and
 * the one sample frame a conversion keeps back to interpolate from. AAC's
 * room is the largest (`aac_core.h`); MP3's frame is 1152 x 2.
 */
#define PB_SRC_SAMPLES (8192 * 8 + 16)

/*
 * An MP3's bytes until they make a frame, and a WAV's until they make a
 * sample frame - records are pieces of the file, and a frame may cross two.
 */
#define PB_CARRY 32768u

/*
 * **How much of an MP3 to have before decoding it**: minimp3, until it has
 * found its first frame, checks the headers of the frames that follow it
 * before it trusts one, and given only part of the stream it skips what it
 * cannot yet check as junk. The MP3 Kit always handed it a 32 KB window of
 * the file; the core decodes only with this much waiting, or at the end.
 */
#define PB_MP3_AHEAD 16384u

struct pb_heard {
    uint64_t periods;                       /* written into the stream's ring */
    uint64_t frames_in;                     /* decoded, at the source's rate */
    uint64_t frames_out;                    /* converted, at the device's */
    uint32_t dry;                           /* passes that wanted a frame and had none,
                                             * once this start's first had come */
    uint32_t refused;                       /* frames that would not decode */
    int      finished;                      /* PB_END reached and all of it written */
};

struct pb {
    enum pb_kind kind;
    long     rate, channels, bits;          /* the source's, once known */
    uint32_t generation;                    /* records of any other are dropped */

    void    *aac;                           /* struct aac *, PB_AAC */
    mp3dec_t mp3;                           /* PB_MP3 */

    uint8_t  carry[PB_CARRY];               /* PB_MP3, PB_WAV: bytes not yet a frame */
    uint32_t carry_n;

    int16_t  src[PB_SRC_SAMPLES];           /* decoded, not yet converted */
    size_t   src_bytes;                     /* of src: a WAV's 8-bit samples are bytes */
    double   phase;                         /* the resampler's */
    int      ended;                         /* PB_END taken: the source is all in src */
    int      fed;                           /* a record of this start taken */

    int16_t  period[8192 / 2];              /* converted, not yet a whole period */
    uint32_t period_fill;                   /* bytes of it */

    struct pb_heard heard;
    const char *why;                        /* the last refusal, for a person */
};

/*
 * A core for `kind`. `rate`, `channels` and `bits` are a WAV's and the
 * MP4's stated ones for AAC (0 where unstated); `config` is AAC's
 * AudioSpecificConfig. 0, or -1 and `p->why`.
 */
int  pb_open(struct pb *p, enum pb_kind kind, long rate, long channels,
             long bits, const uint8_t *config, size_t config_n);
void pb_close(struct pb *p);

/*
 * A seek: the decoder and the resampler forget everything, and from now on
 * only records of `generation` are played.
 */
void pb_restart(struct pb *p, uint32_t generation);

/*
 * **One pass**: periods into `ring` while it has room, from frames in `in`.
 * Returns how many were written. Stops when the ring is full, when the
 * input has nothing more (`heard.dry` counts it if the ring was not), or at
 * the end, where the last period is padded with silence and
 * `heard.finished` is set.
 */
uint32_t pb_pass(struct pb *p, struct pb_input *in, struct audio_ring *ring);

#endif
