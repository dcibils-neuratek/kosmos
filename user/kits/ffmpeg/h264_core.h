/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_H264_CORE_H
#define KOSMOS_H264_CORE_H

/*
 * **H.264, decoded by FFmpeg** (`roadmap.md` 4e): the part of the kit that
 * knows libavcodec, with no Lua in it, so the Mac can hold it to FFmpeg's
 * own conformance checksums (`tools/test_h264.c`) and the kit on top of it
 * (`h264_kosmos.c`) is only the Lua face.
 *
 * A decoder is a conversation rather than a function, which is what makes
 * it unlike the Motion JPEG it sits beside in `/lib/video.lua`: a picture
 * can depend on pictures before and after it, so samples go in in the order
 * they are *decoded* and pictures come out in the order they are *shown*,
 * a few behind. `h264_send` hands over one sample; `h264_receive` answers
 * with a picture when there is one to show. At the end of a film,
 * `h264_finish` lets the last few out; for a seek, `h264_flush` forgets
 * everything and the next sample sent has to be a key frame.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct h264;

/* What `h264_receive` hands back: FFmpeg's planes, valid until the next
 * call on the same decoder. */
struct h264_picture {
    const uint8_t *plane[3];        /* Y, U, V */
    int            stride[3];       /* bytes a row, each its own */
    unsigned       width;
    unsigned       height;
    int64_t        pts;             /* in the timescale the samples used */
    bool           bt709;           /* the colour matrix: 709, else 601 */
    bool           full_range;      /* 0-255 rather than 16-235 */
};

enum h264_status {
    H264_OK = 0,
    H264_AGAIN,                     /* send: take pictures first;
                                     * receive: nothing to show yet */
    H264_END,                       /* receive after finish: all shown */
    H264_ERROR,                     /* see h264_why */
};

/*
 * A decoder for the stream `avcc` describes - the body of the MP4's `avcC`
 * box (14496-15 5.3.3.1), which carries the parameter sets and how long
 * each sample's NAL lengths are. NULL, and why, when there cannot be one.
 */
struct h264 *h264_open(const uint8_t *avcc, size_t n, const char **why);

/* One sample, in decoding order, `n` bytes at `data`, shown at `pts`.
 * Copied: `data` may be reused as soon as this returns. */
enum h264_status h264_send(struct h264 *d, const uint8_t *data, size_t n,
                           int64_t pts);

enum h264_status h264_receive(struct h264 *d, struct h264_picture *out);

/* No more samples: what is held back for reordering comes out. */
enum h264_status h264_finish(struct h264 *d);

/* Everything held, dropped - for a seek. */
void h264_flush(struct h264 *d);

/* The last thing FFmpeg complained about, or why the last call failed. */
const char *h264_why(const struct h264 *d);

void h264_close(struct h264 *d);

#endif /* KOSMOS_H264_CORE_H */
