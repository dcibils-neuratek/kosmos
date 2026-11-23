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
 * them waits but `seek`, which waits for the player's thread to let go
 * of the old ring - a tick or two - so the caller may close its stream.
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

/* Play into `ring` from now, everything before forgotten. False when the
 * ring is not one it can fill, or its thread did not take it in a second. */
bool playback_seek(struct playback *p, struct audio_ring *ring);

bool playback_play(struct playback *p);
bool playback_pause(struct playback *p);

/* The largest frame that would go now. */
size_t playback_room(const struct playback *p);

/*
 * A frame, or a WAV's or an MP3's bytes in pieces of `playback_piece()` at
 * most: 1 taken, 0 the input ring is full, -1 not one it takes, why in
 * `*why`.
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
