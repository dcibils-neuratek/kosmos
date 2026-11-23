/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_PLAYBACK_CORE_H
#define KOSMOS_PLAYBACK_CORE_H

/*
 * **The Playback Kit's core** (`docs/playback.md`): compressed frames in,
 * periods of the device's sound out, into a stream's `audio_ring` - or, for
 * `PB_PCM`, a queue of songs already decoded, trimmed and joined into a
 * stream at their own format (`docs/player.md` P3). Nothing
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
 * the window, which reads the file, or the feeder, which decodes it - and
 * one reader, the thread. Decoded sound's ring is sized in seconds
 * (`pb_input_bytes`); its records are taken a part at a time. The indices
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
#define PB_SONG      0x4u                   /* PB_PCM: a `struct pb_song`, and the song begins */
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

enum pb_kind { PB_WAV = 1, PB_MP3 = 2, PB_PCM = 3 };

/* ------------------------------------------------------------ decoded PCM */

/*
 * **Sound already decoded, a queue of songs** (`docs/player.md` P3). The
 * feeder - the player's thread at NORMAL, decoding through the Media Kit -
 * writes a song record, then that song's samples in records of whole
 * frames, then the next song's record, and so on; `PB_END` after the last.
 *
 * The samples are interleaved, in one of three formats - the contract with
 * the Media Kit's `MEDIA_PCM_*` (`player.md` P1):
 *
 *   PB_PCM_S16  int16_t;
 *   PB_PCM_S32  int32_t, a 24-bit source's sample in the top 24 bits;
 *   PB_PCM_F32  float, full scale at +-1.0 (a lossy decoder's own output).
 *
 * **The stream is at the song's own format**, and the kit converts nothing
 * but the container of a sample: s16 goes out as 16 bits, s32 as 24 when
 * the source had 24 or fewer and as 32 otherwise, f32 as 32-bit integers
 * (`pb_stream_bits`). Rate and channels are the song's. The audio server
 * converts if the device must (`audioproto.h`, U10c), so a 96/24 song
 * reaches a 96/24 device as itself.
 */
#define PB_PCM_S16 1u
#define PB_PCM_S32 2u
#define PB_PCM_F32 3u

/*
 * A song's record. Frames are counted *in the decoded stream* - every frame
 * the decoder makes, the encoder's priming and the decoder's own delay
 * included - so that the feeder states facts and the kit does the trimming:
 *
 *   delay   decoded frames before the song's first: the encoder's delay and
 *           the decoder's together. An MP3 decoded untrimmed is LAME's delay
 *           plus 529 (`PB_MP3_DECODER_DELAY`); an AAC's is `iTunSMPB`'s
 *           priming as written (2112 for Apple's); FLAC's and WAV's are 0.
 *   length  the song's own frames, the padding not among them; 0 when the
 *           file does not say, and then everything after the delay is heard.
 *   first   the decoded-stream index of the first frame that follows this
 *           record: 0 from the start, and where the decoder landed after a
 *           seek in the container.
 *   from    the song's frame to be heard first: 0, or a seek's.
 *
 * Frames before `delay + from` are dropped, and frames from `delay +
 * length` on are the padding and dropped too - unless there are far more of
 * them than any encoder pads (`PB_PAD_MOST`), when the length is wrong and
 * is not believed (FFmpeg writes a Xing frame that says two frames when a
 * cover is attached; P2 found it): then the song plays to its end.
 */
struct pb_song {
    uint32_t song;                          /* the caller's, reported back */
    uint32_t format;                        /* PB_PCM_* */
    uint32_t rate;                          /* 8000 .. 192000 */
    uint32_t channels;                      /* 1 or 2 */
    uint32_t bits;                          /* the source's: 16, 24, 32, or 0 for lossy */
    uint32_t reserved;
    uint64_t delay;
    uint64_t length;
    uint64_t first;
    uint64_t from;
};

_Static_assert(sizeof(struct pb_song) == 56, "a song record is part of the ring's agreement");

#define PB_MP3_DECODER_DELAY 529u           /* FFmpeg's and LAME's: 528 + 1 */
#define PB_PAD_MOST          8192u          /* frames past `length` an encoder may pad */

/* The bits a song's stream is opened at; 0 for a format there is none for. */
static inline uint32_t pb_stream_bits(uint32_t format, uint32_t source_bits)
{
    switch (format) {
    case PB_PCM_S16: return 16;
    case PB_PCM_S32: return source_bits != 0 && source_bits <= 24 ? 24 : 32;
    case PB_PCM_F32: return 32;
    }

    return 0;
}

static inline uint32_t pb_sample_bytes(uint32_t format)
{
    return format == PB_PCM_S16 ? 2u : 4u;
}

/*
 * **The input ring in seconds** (`player.md` §3): bytes for `seconds` of
 * a format's frames, and an eighth more for the records' headers and the
 * skip at the ring's end. Eight seconds of 96/24 stereo, held as int32, is
 * 6.5 MB; of 44.1/16, 1.5 MB.
 */
static inline size_t pb_input_bytes(uint32_t seconds, uint32_t rate, uint32_t channels,
                                    uint32_t format)
{
    uint64_t data = (uint64_t)seconds * rate * channels * pb_sample_bytes(format);

    return (size_t)(64u + data + data / 8u + 65536u);
}

/*
 * Where a song begins in the stream it is heard on: `at` frames into the
 * ring of the `ring`th start (every `pb_restart` and every new ring counts
 * one), heard from its frame `from`. The window compares `at` with the
 * ring's `frames_played` and knows which song is heard, to the frame.
 */
struct pb_mark {
    uint32_t song;
    uint32_t ring;
    uint64_t at;
    uint64_t from;
};

#define PB_MARKS 4u

/*
 * How much of a source's sound is held between frames: a decoded frame and
 * the one sample frame a conversion keeps back to interpolate from. MP3's
 * frame is 1152 x 2; the room was sized for AAC's largest, which went in
 * 0.12.97, and is left as it is until something measures a smaller one.
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

    /* PB_PCM. */
    uint32_t ring;                          /* starts so far: the ring's number */
    uint64_t ring_frames;                   /* frames into this ring, published */
    uint32_t marks;                         /* made so far; the last PB_MARKS kept */
    struct pb_mark mark[PB_MARKS];          /* mark[(marks - 1) % PB_MARKS] is the newest */
    int      change;                        /* a song of another format waits for its ring */
    uint32_t change_rate, change_bits, change_channels;
    uint32_t untrusted;                     /* songs whose stated length was not believed */
};

struct pb {
    enum pb_kind kind;
    long     rate, channels, bits;          /* the source's, once known */
    uint32_t generation;                    /* records of any other are dropped */

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

    /* PB_PCM: the ring's format, and the song being written. */
    uint32_t out_rate, out_bits, out_channels;
    struct pb_song song;
    int      have_song;
    uint64_t index;                         /* decoded-stream index of the next frame */
    uint64_t keep_from, keep_to;            /* of the decoded stream: what is heard */
    int      length_checked;                /* the padding seen to be padding */
    int      marked;                        /* this song's mark made */
    uint32_t rec_at;                        /* bytes of the head record already taken */
    uint32_t slot_fill;                     /* bytes in the ring's slot being filled */

    struct pb_heard heard;
    const char *why;                        /* the last refusal, for a person */
};

/*
 * A core for `kind`. `rate`, `channels` and `bits` are a WAV's (0 for an
 * MP3, which says its own, and for PCM, whose songs say theirs). 0, or -1
 * and `p->why`.
 */
int  pb_open(struct pb *p, enum pb_kind kind, long rate, long channels,
             long bits);
void pb_close(struct pb *p);

/*
 * A seek: the decoder and the resampler forget everything, and from now on
 * only records of `generation` are played.
 */
void pb_restart(struct pb *p, uint32_t generation);

/*
 * PB_PCM: the format of the ring the passes write into from now - after a
 * `pb_restart`, and after `heard.change`, when the caller has opened a
 * stream at the new format. Nothing in the input is forgotten; the ring's
 * frames count from nought again and `heard.ring` goes up by one. False
 * when the format is not one a stream may have (`audioproto.h`) or the
 * ring's period is not whole frames of it.
 */
int  pb_ring_format(struct pb *p, uint32_t rate, uint32_t bits, uint32_t channels,
                    uint32_t period_bytes);

/*
 * **One pass**: periods into `ring` while it has room, from frames in `in`.
 * Returns how many were written. Stops when the ring is full, when the
 * input has nothing more (`heard.dry` counts it if the ring was not), or at
 * the end, where the last period is padded with silence and
 * `heard.finished` is set.
 *
 * PB_PCM stops as well at a song whose format is not the ring's: the period
 * being filled is padded with silence and written, and `heard.change` says
 * the format the next stream must have (`pb_ring_format`). That is the one
 * place a join is not seamless, and it is said.
 */
uint32_t pb_pass(struct pb *p, struct pb_input *in, struct audio_ring *ring);

#endif
