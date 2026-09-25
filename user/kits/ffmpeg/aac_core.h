/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_AAC_CORE_H
#define KOSMOS_AAC_CORE_H

/*
 * **AAC, decoded by FFmpeg** (`roadmap.md` 4e): the sound of nearly every
 * film, with no Lua in it, so the Mac holds it to FFmpeg's own conformance
 * references (`tools/test_aac.c`) and `aac_kosmos.c` is only its Lua face.
 *
 * Unlike H.264 this is one frame in, one frame out: an AAC frame is 1024
 * samples a channel (2048 with SBR, HE-AAC's doubling of the rate), decoded
 * from one sample of the file and depending only on the frame before it.
 * What comes back is sixteen-bit and interleaved: every channel the stream
 * has, which is what a comparison with the reference needs, or two, which
 * is what the device takes.
 */

#include <stddef.h>
#include <stdint.h>

struct aac;

/*
 * A decoder for the stream `config` describes - the AudioSpecificConfig
 * (14496-3 1.6.2.1) from the MP4's `esds` - with the rate and channel
 * count the MP4's sample entry states, 0 where it states none. NULL, and
 * why, when there cannot be one.
 *
 * The two numbers are what FFmpeg's own MP4 reader hands its decoder
 * before the first frame, and a low-delay stream (ER AAC-LD) has nothing
 * else to go on: without them it decodes a frame with no rate, which
 * FFmpeg then refuses as invalid (`tools/test_aac.c` found it).
 */
struct aac *aac_open(const uint8_t *config, size_t n, unsigned rate,
                     unsigned channels, const char **why);

/*
 * One frame of the file, `n` bytes at `data`, into `out`: `*samples` a
 * channel, `*channels` interleaved, at `*rate`. `room` is how many int16
 * `out` holds; `AAC_ROOM` is enough for anything AAC makes. 0 on success,
 * -1 and `aac_why` when the frame would not decode - a damaged frame,
 * which the caller skips.
 *
 * `stereo` makes more than two channels into two, as ITU-R BS.775 does:
 * centre and surrounds at -3 dB, the LFE left out, scaled so that nothing
 * clips. Here rather than after, because only the decoder knows which
 * channel is which - FFmpeg hands them back in its own order, not AAC's.
 * Mono and stereo are left alone.
 */
#define AAC_ROOM (8192 * 8)

int aac_decode(struct aac *d, const uint8_t *data, size_t n, int16_t *out,
               size_t room, int stereo, unsigned *samples,
               unsigned *channels, unsigned *rate);

/*
 * What channel `i` of the last frame is, as FFmpeg names it - "FL", "FC",
 * "LFE", "BL" and so on - or NULL past the last. Every channel, in the
 * order `aac_decode` interleaves them without `stereo`; for a test to mix
 * the reference by, and for anybody who wants to say what a film carries.
 */
const char *aac_channel(const struct aac *d, unsigned i);

/* Forget the frame before: a seek. */
void aac_reset(struct aac *d);

const char *aac_why(const struct aac *d);

void aac_close(struct aac *d);

#endif /* KOSMOS_AAC_CORE_H */
