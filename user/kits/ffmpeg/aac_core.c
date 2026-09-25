/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * AAC through libavcodec. `aac_core.h` says what it is for; this is the
 * libavcodec side of it.
 *
 * FFmpeg decodes AAC to planar floats, a plane a channel, and this turns
 * them into what the rest of the sound path takes: sixteen-bit samples,
 * interleaved - the conversion FFmpeg's own command line makes, `x * 32768`
 * rounded and clipped, so the Mac can hold this to FFmpeg's references.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/mem.h"
#include "libavutil/samplefmt.h"

#include "aac_core.h"
#include "ffmpeg_log.h"

struct aac {
    AVCodecContext *ctx;
    AVPacket       *packet;
    AVFrame        *frame;
    const char     *why;
    char            said[160];
    AVChannelLayout layout;             /* the last frame's */
    char            names[64][16];
};

static int failed(struct aac *d, int code, const char *what)
{
    char because[64];

    if (ffmpeg_log_said()[0] != '\0') {
        snprintf(d->said, sizeof d->said, "%s: %s", what, ffmpeg_log_said());
    } else {
        av_strerror(code, because, sizeof because);
        snprintf(d->said, sizeof d->said, "%s: %s", what, because);
    }
    d->why = d->said;
    return -1;
}

struct aac *aac_open(const uint8_t *config, size_t n, unsigned rate,
                     unsigned channels, const char **why)
{
    const AVCodec *codec;
    struct aac *d;

    ffmpeg_log_keep();
    ffmpeg_log_clear();

    /* Two bytes is the shortest AudioSpecificConfig there is: an object
     * type, a rate index and a channel configuration. */
    if (config == NULL || n < 2) {
        *why = "the film does not describe its AAC stream (no config)";
        return NULL;
    }

    codec = avcodec_find_decoder(AV_CODEC_ID_AAC);

    if (codec == NULL) {
        *why = "this system was built without the AAC decoder";
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
        aac_close(d);
        *why = "no memory for a decoder";
        return NULL;
    }

    /* The AudioSpecificConfig is what an MP4 carries instead of a header
     * on every frame, as `avcC` is for H.264; padded, as FFmpeg wants
     * every buffer it reads. */
    d->ctx->extradata = av_mallocz(n + AV_INPUT_BUFFER_PADDING_SIZE);

    if (d->ctx->extradata == NULL) {
        aac_close(d);
        *why = "no memory for a decoder";
        return NULL;
    }

    memcpy(d->ctx->extradata, config, n);
    d->ctx->extradata_size = (int)n;
    d->ctx->thread_count = 1;

    /* What the sample entry says, as FFmpeg's MP4 reader passes it on
     * (`aac_core.h`); the config overrides both wherever it says more. */
    if (rate > 0 && rate <= 384000) {
        d->ctx->sample_rate = (int)rate;
    }
    if (channels > 0 && channels <= 64) {
        av_channel_layout_default(&d->ctx->ch_layout, (int)channels);
    }

    if (avcodec_open2(d->ctx, codec, NULL) < 0) {
        static char because[160];

        failed(d, AVERROR_INVALIDDATA, "the AAC stream would not open");
        snprintf(because, sizeof because, "%s", d->said);
        aac_close(d);
        *why = because;
        return NULL;
    }

    return d;
}

/* To sixteen bits as `swresample` does it: times 32768, rounded, clipped.
 * Rounding half away from zero where it rounds half to even, which differs
 * by one at an exact half - within what FFmpeg's own test allows. */
static int16_t s16(float x)
{
    float v = x * 32768.0f;

    if (v >= 32767.0f) {
        return 32767;
    }
    if (v <= -32768.0f) {
        return -32768;
    }
    return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

/*
 * How much of channel `i` goes to the left and to the right, by what the
 * layout says it is. ITU-R BS.775: centre and surrounds at -3 dB, a back
 * centre at -6 dB into each side, the LFE left out.
 */
static void weights(const AVChannelLayout *layout, int i, float *l, float *r)
{
    enum AVChannel c = av_channel_layout_channel_from_index(layout, (unsigned)i);
    const float half = 0.70710678f;

    *l = 0.0f;
    *r = 0.0f;

    switch (c) {
    case AV_CHAN_FRONT_LEFT:
    case AV_CHAN_FRONT_LEFT_OF_CENTER:
        *l = 1.0f;
        break;
    case AV_CHAN_FRONT_RIGHT:
    case AV_CHAN_FRONT_RIGHT_OF_CENTER:
        *r = 1.0f;
        break;
    case AV_CHAN_FRONT_CENTER:
        *l = half;
        *r = half;
        break;
    case AV_CHAN_BACK_LEFT:
    case AV_CHAN_SIDE_LEFT:
    case AV_CHAN_WIDE_LEFT:
    case AV_CHAN_SURROUND_DIRECT_LEFT:
        *l = half;
        break;
    case AV_CHAN_BACK_RIGHT:
    case AV_CHAN_SIDE_RIGHT:
    case AV_CHAN_WIDE_RIGHT:
    case AV_CHAN_SURROUND_DIRECT_RIGHT:
        *r = half;
        break;
    case AV_CHAN_BACK_CENTER:
        *l = 0.5f;
        *r = 0.5f;
        break;
    default:
        /* The LFE, the height channels, and a layout that names nothing:
         * for that last, the first two channels are left and right. */
        if (layout->order == AV_CHANNEL_ORDER_UNSPEC) {
            *l = (i == 0) ? 1.0f : 0.0f;
            *r = (i == 1) ? 1.0f : 0.0f;
        }
        break;
    }
}

/* The sample `s` of channel `c`, planar or interleaved. */
static float at(const AVFrame *f, bool planar, int channels, int c, int s)
{
    return planar ? ((const float *)f->extended_data[c])[s]
                  : ((const float *)f->extended_data[0])[s * channels + c];
}

int aac_decode(struct aac *d, const uint8_t *data, size_t n, int16_t *out,
               size_t room, int stereo, unsigned *samples,
               unsigned *channels, unsigned *rate)
{
    const AVFrame *f;
    enum AVSampleFormat format;
    bool planar;
    int r, count, chans, s, c;

    d->why = NULL;
    ffmpeg_log_clear();
    *samples = 0;

    if (n > (size_t)INT32_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
        d->why = "a sample too large to be a frame";
        return -1;
    }

    r = av_new_packet(d->packet, (int)n);

    if (r < 0) {
        return failed(d, r, "no memory for a sample");
    }

    memcpy(d->packet->data, data, n);
    r = avcodec_send_packet(d->ctx, d->packet);
    av_packet_unref(d->packet);

    if (r < 0) {
        return failed(d, r, "the frame would not decode");
    }

    r = avcodec_receive_frame(d->ctx, d->frame);

    if (r == AVERROR(EAGAIN)) {
        return 0;                       /* nothing yet, which is not wrong */
    }

    if (r < 0) {
        return failed(d, r, "no sound came out");
    }

    f = d->frame;
    format = (enum AVSampleFormat)f->format;
    planar = format == AV_SAMPLE_FMT_FLTP;
    count = f->nb_samples;
    chans = f->ch_layout.nb_channels;

    if ((format != AV_SAMPLE_FMT_FLTP && format != AV_SAMPLE_FMT_FLT)
        || chans < 1) {
        av_frame_unref(d->frame);
        d->why = "the decoder gave back samples of a kind this cannot use";
        return -1;
    }

    if (stereo && chans > 2) {
        float wl[64], wr[64], sum_l = 0.0f, sum_r = 0.0f, scale;

        if (chans > 64 || (size_t)count * 2u > room) {
            av_frame_unref(d->frame);
            d->why = "a frame larger than there is room for";
            return -1;
        }

        for (c = 0; c < chans; c++) {
            weights(&f->ch_layout, c, &wl[c], &wr[c]);
            sum_l += wl[c];
            sum_r += wr[c];
        }

        /* Loud everywhere at once still fits. */
        scale = sum_l > sum_r ? sum_l : sum_r;
        scale = scale > 1.0f ? 1.0f / scale : 1.0f;

        for (s = 0; s < count; s++) {
            float l = 0.0f, rr = 0.0f;

            for (c = 0; c < chans; c++) {
                float x = at(f, planar, chans, c, s);

                l += wl[c] * x;
                rr += wr[c] * x;
            }

            out[2 * s] = s16(l * scale);
            out[2 * s + 1] = s16(rr * scale);
        }

        chans = 2;
    } else {
        if ((size_t)count * (size_t)chans > room) {
            av_frame_unref(d->frame);
            d->why = "a frame larger than there is room for";
            return -1;
        }

        for (s = 0; s < count; s++) {
            for (c = 0; c < chans; c++) {
                out[s * chans + c] = s16(at(f, planar, chans, c, s));
            }
        }
    }

    *samples = (unsigned)count;
    *channels = (unsigned)chans;
    *rate = (unsigned)f->sample_rate;
    av_channel_layout_uninit(&d->layout);
    av_channel_layout_copy(&d->layout, &f->ch_layout);
    av_frame_unref(d->frame);
    return 0;
}

const char *aac_channel(const struct aac *d, unsigned i)
{
    struct aac *w = (struct aac *)d;

    if (i >= (unsigned)d->layout.nb_channels || i >= 64) {
        return NULL;
    }

    av_channel_name(w->names[i], sizeof w->names[i],
                    av_channel_layout_channel_from_index(&d->layout, i));
    return w->names[i];
}

void aac_reset(struct aac *d)
{
    avcodec_flush_buffers(d->ctx);
}

const char *aac_why(const struct aac *d)
{
    if (d->why != NULL) {
        return d->why;
    }

    return ffmpeg_log_said()[0] != '\0' ? ffmpeg_log_said()
                                        : "no reason was given";
}

void aac_close(struct aac *d)
{
    if (d == NULL) {
        return;
    }

    av_channel_layout_uninit(&d->layout);
    av_frame_free(&d->frame);
    av_packet_free(&d->packet);
    avcodec_free_context(&d->ctx);
    av_free(d);
}
