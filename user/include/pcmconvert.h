/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_PCMCONVERT_H
#define KOSMOS_PCMCONVERT_H

/*
 * **Somebody else's samples, in this machine's format** - the one copy.
 *
 * `sys.pcm` was this, inside `sys_user.c`; the Playback Kit needs the same
 * conversion on its thread (`docs/playback.md`, step 1), and two copies of a
 * resampler drift. So it lives here, and `sys.pcm` and the kit both call it.
 * `tools/test_pcmconvert.c` holds it on the Mac.
 *
 * In: 8-bit unsigned or 16-bit signed little-endian, mono or stereo, any
 * rate. Out: always the device's - 16-bit stereo at `PCM_DEVICE_RATE`.
 *
 * **Linear interpolation** between neighbouring frames: not nearest, which
 * is audibly wrong on anything bright, and not a windowed sinc, which is a
 * great deal more arithmetic for a difference a laptop speaker does not
 * show. Saying which matters more than which.
 *
 * **The phase is the caller's**, carried from one piece to the next: a file
 * is converted a piece at a time, the position between input frames does not
 * end on a whole number, and starting each piece at nought puts a click at
 * every boundary.
 *
 * **`last`** says these bytes are the end of the input. Every frame but the
 * final one is interpolated towards the one after it, so without this the
 * final frame waited for a neighbour that never came, and the last sample of
 * every song and film went unplayed (`testing.md` 18.185). With it, the final
 * frame is its own neighbour.
 *
 * Returns the frames written to `out` (two int16 each), at most
 * `max_out_bytes / 4`, and sets `*consumed` to the input bytes finished
 * with - everything before the frame the phase now sits in, which the next
 * piece interpolates from - and `*phase` reduced by as many frames. Nought
 * frames and nought consumed when there is not enough to start (two frames,
 * or one when `last`). The caller has checked the format (`pcm_format_ok`).
 */

#include <stddef.h>
#include <stdint.h>

#define PCM_DEVICE_RATE 44100u

static inline int pcm_format_ok(long rate, long channels, long bits)
{
    return rate > 0 && (channels == 1 || channels == 2)
           && (bits == 8 || bits == 16);
}

static inline long pcm_convert(const unsigned char *in, size_t len,
                               long rate, long channels, long bits, int last,
                               double *phase_io, int16_t *out,
                               long max_out_bytes, size_t *consumed_out)
{
    long in_frame_bytes = channels * (bits / 8);
    long in_frames = (long)(len / (size_t)in_frame_bytes);
    long out_frames = 0;
    double phase = *phase_io;
    double step;

    *consumed_out = 0;

    if (in_frames < (last ? 1 : 2)) {
        return 0;
    }

    step = (double)rate / (double)PCM_DEVICE_RATE;

    while (out_frames * 4 + 4 <= max_out_bytes) {
        long at = (long)phase;
        double frac = phase - (double)at;
        int32_t l0, r0, l1, r1;

        /* The last frame has no neighbour to interpolate towards, so it is
         * where this piece stops - unless there is no next piece. */
        if (at + 1 >= in_frames && !(last && at + 1 == in_frames)) {
            break;
        }

        {
            const unsigned char *a = in + at * in_frame_bytes;
            const unsigned char *b = (at + 1 < in_frames)
                                     ? a + in_frame_bytes : a;

            if (bits == 8) {
                /* Unsigned, centred on 128, which is what an 8-bit WAV is. */
                l0 = ((int32_t)a[0] - 128) << 8;
                r0 = (channels == 2) ? (((int32_t)a[1] - 128) << 8) : l0;
                l1 = ((int32_t)b[0] - 128) << 8;
                r1 = (channels == 2) ? (((int32_t)b[1] - 128) << 8) : l1;
            } else {
                /* A byte at a time: nothing promises two-byte alignment. */
                l0 = (int16_t)(a[0] | (a[1] << 8));
                r0 = (channels == 2) ? (int16_t)(a[2] | (a[3] << 8)) : l0;
                l1 = (int16_t)(b[0] | (b[1] << 8));
                r1 = (channels == 2) ? (int16_t)(b[2] | (b[3] << 8)) : l1;
            }
        }

        out[out_frames * 2]     = (int16_t)(l0 + (l1 - l0) * frac);
        out[out_frames * 2 + 1] = (int16_t)(r0 + (r1 - r0) * frac);

        out_frames++;
        phase += step;
    }

    {
        long consumed = (long)phase;

        /* All of it at the end; otherwise the frame the phase sits in stays
         * for the next piece to interpolate from. */
        if (consumed > in_frames - (last ? 0 : 1)) {
            consumed = in_frames - (last ? 0 : 1);
        }

        if (consumed < 0) {
            consumed = 0;
        }

        *phase_io = phase - (double)consumed;
        *consumed_out = (size_t)(consumed * in_frame_bytes);
    }

    return out_frames;
}

#endif
