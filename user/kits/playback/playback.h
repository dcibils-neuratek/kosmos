/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Playback Kit's door from C** (`docs/playback.md`, `docs/media.md`
 * M4): a player - its thread in the audio band, its input ring, its
 * commands - for code without a `lua_State`. `playback_kosmos.c` is the
 * same player for Lua, and is only this door with Lua's arguments around
 * it, so the two cannot drift.
 *
 * The order a caller follows: `open`, then `seek` with the ring of a
 * stream it opened (the first start and every seek), `frame`s, `play`;
 * `ended` once no frame will follow; `state` to see what is heard;
 * `close`. Every call but `close` is the caller's thread's and none of
 * them waits but `seek` and `continue`, which wait for the player's thread
 * to let go of the old ring - a tick or two - so the caller may close its
 * stream.
 *
 * **Decoded sound, a queue of songs** (`docs/player.md` P3): `open_pcm`,
 * then `seek_format` with a stream opened at the first song's format
 * (`playback_stream_bits`), then for each song a `song` and its samples as
 * `frame`s of whole frames, and `ended` after the last. Songs of one format
 * join sample for sample, their encoders' delay and padding trimmed; a song
 * of another format stops the player at its first frame with `change` set
 * in the state, and `continue` with a stream at that format goes on. Which
 * song is heard, to the frame, is in the state's marks.
 */
#ifndef KOSMOS_PLAYBACK_H
#define KOSMOS_PLAYBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audioring.h"

#ifdef __cplusplus
extern "C" {
#endif

struct playback;

/* The kinds, `playback_core.h`'s `enum pb_kind` by number, so a caller
 * needs only this header. A WAV's bytes are PCM: 8 or 16 bits, any rate,
 * one channel or two. */
#define PLAYBACK_WAV 1
#define PLAYBACK_MP3 2
#define PLAYBACK_PCM 3              /* decoded: `playback_open_pcm` */

/*
 * **Decoded samples, interleaved** - the contract with the Media Kit's
 * `MEDIA_PCM_*` (`docs/player.md` P1): int16; int32 with a 24-bit source's
 * sample in its top 24 bits; float at +-1.0. `playback_core.h`'s
 * `PB_PCM_*` by number.
 */
#define PLAYBACK_PCM_S16 1
#define PLAYBACK_PCM_S32 2
#define PLAYBACK_PCM_F32 3

/* An MP3 decoded untrimmed starts this much later than LAME's delay says. */
#define PLAYBACK_MP3_DECODER_DELAY 529

/* How far ahead the input ring holds, unless the caller says (`player.md` §3). */
#define PLAYBACK_AHEAD_SECONDS 8

/*
 * A song, before its samples (`playback_core.h`'s `struct pb_song`, which
 * says what each count means). Frames are counted in the decoded stream,
 * delay included: an MP3 decoded untrimmed has `delay` = LAME's delay +
 * `PLAYBACK_MP3_DECODER_DELAY`, an AAC `iTunSMPB`'s priming, FLAC and WAV
 * 0; `length` the song's own frames, or 0 when the file does not say.
 */
struct playback_song {
    uint32_t song;                  /* the caller's: comes back in the marks */
    uint32_t format;                /* PLAYBACK_PCM_* */
    uint32_t rate;
    uint32_t channels;              /* 1 or 2 */
    uint32_t bits;                  /* the source's: 16, 24, 32; 0 for lossy */
    uint32_t reserved;
    uint64_t delay;
    uint64_t length;
    uint64_t first;                 /* decoded index of the first frame to follow */
    uint64_t from;                  /* the song's frame to be heard first */
};

/* Where a song begins: `at` frames into the stream of start `ring`, from its frame `from`. */
struct playback_mark {
    uint32_t song;
    uint32_t ring;
    uint64_t at;
    uint64_t from;
};

/* What is heard, copied from the player's thread. */
struct playback_state {
    uint64_t periods;           /* written into the stream's ring */
    uint64_t frames_in;         /* decoded, at the source's rate */
    uint64_t frames_out;        /* converted, at the device's */
    uint32_t dry;               /* passes that wanted a frame and had none */
    uint32_t refused;           /* frames that would not decode */
    bool     finished;          /* the end, all of it written, since the last seek */
    bool     playing;
    bool     audio_band;        /* its thread is in the audio band */
    unsigned long worst_pass;   /* counter ticks, the longest away while playing */
    char     why[96];

    /* PLAYBACK_PCM. */
    uint32_t ring;              /* the start now: one more at every seek and continue */
    uint64_t ring_frames;       /* frames written into this start's ring */
    uint32_t marks;             /* songs begun so far */
    struct playback_mark mark[2];   /* [0] the newest, [1] the one before */
    bool     change;            /* a song of another format waits: `continue` */
    uint32_t change_rate, change_bits, change_channels;
    uint32_t untrusted;         /* songs whose stated length was not believed */
};

/*
 * A player for `kind`, its thread started and paused. NULL, and why in
 * `*why`, when the sound cannot be played or there is no memory or thread.
 */
/*
 * `config` and `config_n` were AAC's AudioSpecificConfig, and are unused
 * since AAC left this kit in 0.12.97 (`docs/media.md` M6d). They stay only
 * because WebKit's port (`MediaPlayerPrivateKosmos.cpp`) passes them, and
 * go the next time WebKit is built (`roadmap.md`).
 */
struct playback *playback_open(int kind, long rate, long channels,
                               long bits, const uint8_t *config, size_t config_n,
                               const char **why);

/*
 * A player of decoded songs (`PLAYBACK_PCM`): its input ring holds
 * `seconds` (0: `PLAYBACK_AHEAD_SECONDS`) of `format` at `rate` and
 * `channels` - the largest the queue will play; a larger song is played
 * with fewer seconds ahead.
 */
struct playback *playback_open_pcm(int format, long rate, long channels,
                                   unsigned seconds, const char **why);

/* The bits a song's stream is opened at (`AUDIO_OP_OPEN`); 0 for no such format. */
long playback_stream_bits(int format, long source_bits);

/* Play into `ring` from now, everything before forgotten. False when the
 * ring is not one it can fill, or its thread did not take it in a second.
 * A WAV's and an MP3's ring is format 0's, 16-bit stereo at the mix's rate. */
bool playback_seek(struct playback *p, struct audio_ring *ring);

/*
 * The same for decoded songs, with the stream's format - `rate`, `bits`
 * (16, 24 or 32) and `channels`, as the stream was opened. Frames that
 * follow start with a `song`.
 */
bool playback_seek_format(struct playback *p, struct audio_ring *ring,
                          long rate, long bits, long channels);

/*
 * After `change`: into this ring, at the new format, from where it stopped,
 * nothing forgotten. Waits as `seek` does, so the old stream may be closed.
 */
bool playback_continue(struct playback *p, struct audio_ring *ring,
                       long rate, long bits, long channels);

/* A song begins: 1 taken, 0 the input ring is full, -1 not one it plays. */
int playback_song(struct playback *p, const struct playback_song *song, const char **why);

bool playback_play(struct playback *p);
bool playback_pause(struct playback *p);

/* The largest frame that would go now. */
size_t playback_room(const struct playback *p);

/*
 * A frame, or a WAV's or an MP3's bytes in pieces of `playback_piece()` at
 * most, or a song's samples in whole frames of its format: 1 taken, 0 the
 * input ring is full, -1 not one it takes, why in `*why`.
 */
int playback_frame(struct playback *p, const void *bytes, size_t n, const char **why);
size_t playback_piece(const struct playback *p);

/* No frame will follow these: true, or false while the input ring is full. */
bool playback_ended(struct playback *p);

void playback_state(const struct playback *p, struct playback_state *out);

/* The thread stopped and everything freed. */
void playback_close(struct playback *p);

#ifdef __cplusplus
}
#endif

#endif
