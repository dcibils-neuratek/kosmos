/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * H.264 through libavcodec. `h264_core.h` says what the conversation is;
 * this is the libavcodec side of it, and the only file in Kosmos that
 * includes an FFmpeg header.
 *
 * **One thread, deliberately.** FFmpeg can decode slices or frames on
 * several threads, and it is built here without any (`--disable-pthreads`
 * in `tools/ffmpeg_vendor.py`): a Kosmos process has one thread until
 * `docs/threads.md` says otherwise, and a decoder that wants four cores is
 * a thing to measure first. What a film costs is in its overlay, and that
 * is where the case for threads will come from.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"

#include "h264_core.h"

struct h264 {
    AVCodecContext *ctx;
    AVPacket       *packet;
    AVFrame        *frame;
    const char     *why;
    char            said[160];
};

/*
 * What FFmpeg says, kept rather than printed.
 *
 * Its default is `fprintf(stderr, ...)`, which here would be the console of
 * whatever process decodes - and a damaged film says something about every
 * damaged slice, which is dozens of lines a second. So the last complaint
 * at error level is kept, and `h264_why` answers with it when a caller
 * asks why a sample did not decode. One buffer for the process: FFmpeg's
 * log is process-wide, and so is this.
 */
static char last_said[160];

static void keep(void *avcl, int level, const char *format, va_list vl)
{
    size_t n;

    (void)avcl;

    if (level > AV_LOG_ERROR) {
        return;
    }

    vsnprintf(last_said, sizeof last_said, format, vl);
    n = strlen(last_said);

    while (n > 0 && (last_said[n - 1] == '\n' || last_said[n - 1] == ' ')) {
        last_said[--n] = '\0';
    }
}

static enum h264_status failed(struct h264 *d, int code, const char *what)
{
    char because[64];

    if (last_said[0] != '\0') {
        snprintf(d->said, sizeof d->said, "%s: %s", what, last_said);
    } else {
        av_strerror(code, because, sizeof because);
        snprintf(d->said, sizeof d->said, "%s: %s", what, because);
    }
    d->why = d->said;
    return H264_ERROR;
}

struct h264 *h264_open(const uint8_t *avcc, size_t n, const char **why)
{
    const AVCodec *codec;
    struct h264 *d;

    av_log_set_callback(keep);
    last_said[0] = '\0';

    /* `avcC` begins with its version, 1; anything else is not one. */
    if (avcc == NULL || n < 7 || avcc[0] != 1) {
        *why = "the film does not describe its H.264 stream (no avcC)";
        return NULL;
    }

    codec = avcodec_find_decoder(AV_CODEC_ID_H264);

    if (codec == NULL) {
        *why = "this system was built without the H.264 decoder";
        return NULL;
    }

    d = av_mallocz(sizeof *d);

    if (d == NULL) {
        *why = "no memory for a decoder";
        return NULL;
    }

    d->ctx = avcodec_alloc_context3(codec);
    d->packet = av_packet_alloc();
    d->frame = av_frame_alloc();

    if (d->ctx == NULL || d->packet == NULL || d->frame == NULL) {
        h264_close(d);
        *why = "no memory for a decoder";
        return NULL;
    }

    /*
     * The parameter sets and the length of each NAL unit's size, which is
     * what makes the samples readable at all: an MP4's H.264 is length-
     * prefixed rather than Annex B's start codes, and only `avcC` says how
     * long the prefix is. FFmpeg wants it padded, like every buffer it
     * reads.
     */
    d->ctx->extradata = av_mallocz(n + AV_INPUT_BUFFER_PADDING_SIZE);

    if (d->ctx->extradata == NULL) {
        h264_close(d);
        *why = "no memory for a decoder";
        return NULL;
    }

    memcpy(d->ctx->extradata, avcc, n);
    d->ctx->extradata_size = (int)n;
    d->ctx->thread_count = 1;

    /*
     * **How far pictures are reordered, from the standard rather than a
     * guess.** A stream whose parameter sets do not say starts, by FFmpeg's
     * default, assuming no reordering at all, and discards the pictures it
     * later finds came out too early - the first two of a film with
     * B-frames, gone. FFmpeg's own player never meets this because it
     * decodes the start of a file to probe it before playing; a kit handed
     * samples does not. `STRICT` has the decoder take the depth the level
     * allows (H.264 A.3.1), which delays the first picture by a few and
     * loses none. The one other thing it changes is a check on HDR
     * metadata this kit does not read. `tools/test_h264.c` found it: two
     * conformance streams two pictures short.
     */
    d->ctx->strict_std_compliance = FF_COMPLIANCE_STRICT;

    /*
     * **Cropping to the pixel.** A stream may say its picture starts some
     * columns in, and FFmpeg crops the left edge only as far as keeps the
     * planes aligned for its vector code - unless told the caller does not
     * need that. `gfx_draw_i420` reads from any address, so the picture is
     * cropped where the film says: 300 wide, and not 326 with the edge in
     * it (`CVFC1_Sony_C`, which found it).
     */
    d->ctx->flags |= AV_CODEC_FLAG_UNALIGNED;

    if (avcodec_open2(d->ctx, codec, NULL) < 0) {
        failed(d, AVERROR_INVALIDDATA, "the H.264 stream would not open");
        snprintf(last_said, sizeof last_said, "%s", d->said);
        h264_close(d);
        *why = last_said;
        return NULL;
    }

    return d;
}

enum h264_status h264_send(struct h264 *d, const uint8_t *data, size_t n,
                           int64_t pts)
{
    int r;

    d->why = NULL;
    last_said[0] = '\0';

    /*
     * Into a packet of FFmpeg's own, because it reads up to
     * `AV_INPUT_BUFFER_PADDING_SIZE` bytes past the end of what it is given
     * and they must be zeros. The caller's bytes are in a read buffer that
     * ends wherever the sample happened to - possibly at the end of a
     * mapping. A sample is tens of kilobytes; copying it is nothing next
     * to decoding it.
     */
    if (n > (size_t)INT32_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
        d->why = "a sample too large to be a frame";
        return H264_ERROR;
    }

    r = av_new_packet(d->packet, (int)n);

    if (r < 0) {
        return failed(d, r, "no memory for a sample");
    }

    memcpy(d->packet->data, data, n);
    d->packet->pts = pts;
    d->packet->dts = AV_NOPTS_VALUE;

    r = avcodec_send_packet(d->ctx, d->packet);
    av_packet_unref(d->packet);

    if (r == AVERROR(EAGAIN)) {
        return H264_AGAIN;
    }

    if (r < 0) {
        return failed(d, r, "the sample would not decode");
    }

    return H264_OK;
}

enum h264_status h264_receive(struct h264 *d, struct h264_picture *out)
{
    enum AVPixelFormat format;
    int r;

    d->why = NULL;
    r = avcodec_receive_frame(d->ctx, d->frame);

    if (r == AVERROR(EAGAIN)) {
        return H264_AGAIN;
    }

    if (r == AVERROR_EOF) {
        return H264_END;
    }

    if (r < 0) {
        return failed(d, r, "no picture came out");
    }

    /*
     * Eight bits, 4:2:0: every camera, every phone, every film on the web.
     * High 10 and the 4:2:2 and 4:4:4 profiles decode here and then have no
     * converter to go to, so they are refused by name rather than drawn as
     * noise.
     */
    format = (enum AVPixelFormat)d->frame->format;

    if (format != AV_PIX_FMT_YUV420P && format != AV_PIX_FMT_YUVJ420P) {
        const char *name = av_get_pix_fmt_name(format);

        snprintf(d->said, sizeof d->said, "this film is %s, and only 8-bit "
                 "4:2:0 (yuv420p) can be shown", name ? name : "unknown");
        d->why = d->said;
        av_frame_unref(d->frame);
        return H264_ERROR;
    }

    out->plane[0] = d->frame->data[0];
    out->plane[1] = d->frame->data[1];
    out->plane[2] = d->frame->data[2];
    out->stride[0] = d->frame->linesize[0];
    out->stride[1] = d->frame->linesize[1];
    out->stride[2] = d->frame->linesize[2];
    out->width = (unsigned)d->frame->width;
    out->height = (unsigned)d->frame->height;
    out->pts = d->frame->best_effort_timestamp;

    /*
     * Which matrix the film was made with. It says so when it says
     * anything; when it does not, the convention every player follows is
     * the one its size implies - BT.709 for high definition, BT.601 for
     * what came before it.
     */
    if (d->frame->colorspace == AVCOL_SPC_BT709) {
        out->bt709 = true;
    } else if (d->frame->colorspace == AVCOL_SPC_UNSPECIFIED) {
        out->bt709 = d->frame->height >= 720;
    } else {
        out->bt709 = false;
    }

    out->full_range = d->frame->color_range == AVCOL_RANGE_JPEG
                      || format == AV_PIX_FMT_YUVJ420P;
    return H264_OK;
}

enum h264_status h264_finish(struct h264 *d)
{
    int r = avcodec_send_packet(d->ctx, NULL);

    if (r < 0 && r != AVERROR_EOF) {
        return failed(d, r, "the film would not finish");
    }

    return H264_OK;
}

void h264_flush(struct h264 *d)
{
    avcodec_flush_buffers(d->ctx);
    av_frame_unref(d->frame);
}

const char *h264_why(const struct h264 *d)
{
    if (d->why != NULL) {
        return d->why;
    }

    return last_said[0] != '\0' ? last_said : "no reason was given";
}

void h264_close(struct h264 *d)
{
    if (d == NULL) {
        return;
    }

    av_frame_free(&d->frame);
    av_packet_free(&d->packet);
    avcodec_free_context(&d->ctx);
    av_free(d);
}
