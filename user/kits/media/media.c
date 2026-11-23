/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Media Kit** (`media.h`, `docs/media.md` M1), over FFmpeg's
 * libavformat, libavcodec and libswresample. Everything here is the
 * translation between FFmpeg's terms and the header's: the caller's
 * reader as an AVIOContext, a container's time base as microseconds, a
 * decoder's frame as planes, as interleaved float, or as interleaved
 * samples in the decoder's own format (`media_sound`).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/cpu.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>

#include "media.h"

#define IO_BUFFER 65536

struct media_source {
    struct media_io io;
    AVFormatContext *fmt;
    AVIOContext *avio;
};

struct media_decoder {
    AVCodecContext *ctx;
    AVRational tb;
    AVFrame *frame;
    int kind;
    /* sound: the converter to float, and what it was made for */
    SwrContext *swr;
    int swr_format, swr_rate;
    AVChannelLayout swr_layout;
    float *pcm;
    int pcm_room;
    /* sound at its own resolution (`media_sound`): the source's bits, and
     * the frame made interleaved, then converted when asked */
    int source_bits;
    void *native;
    size_t native_room;
    void *asked;
    size_t asked_room;
};

static void say(char *why, size_t room, const char *what, int code)
{
    char e[128];

    if (!why || !room)
        return;
    if (code) {
        av_strerror(code, e, sizeof e);
        snprintf(why, room, "%s: %s", what, e);
    } else {
        snprintf(why, room, "%s", what);
    }
}

static int64_t to_us(int64_t t, AVRational tb)
{
    if (t == AV_NOPTS_VALUE)
        return MEDIA_NO_TIME;
    return av_rescale_q(t, tb, AV_TIME_BASE_Q);
}

static int64_t from_us(int64_t us, AVRational tb)
{
    if (us == MEDIA_NO_TIME)
        return AV_NOPTS_VALUE;
    return av_rescale_q(us, AV_TIME_BASE_Q, tb);
}

/* The caller's reader, as FFmpeg asks for it. */
static int io_read(void *opaque, uint8_t *buf, int n)
{
    struct media_source *s = opaque;
    long got = s->io.read(s->io.ctx, buf, (size_t)n);

    if (got == 0)
        return AVERROR_EOF;
    if (got < 0)
        return AVERROR(EIO);
    return (int)got;
}

static int64_t io_seek(void *opaque, int64_t offset, int whence)
{
    struct media_source *s = opaque;

    if (whence & AVSEEK_SIZE)
        return s->io.size >= 0 ? s->io.size : AVERROR(ENOSYS);
    whence &= ~AVSEEK_FORCE;
    if (!s->io.seek)
        return AVERROR(ENOSYS);
    return s->io.seek(s->io.ctx, offset, whence);
}

const char *media_version(void)
{
    return av_version_info();
}

int media_cpu_flags(int mask)
{
    av_force_cpu_flags(-1);
    if (mask != -1)
        av_force_cpu_flags(av_get_cpu_flags() & mask);
    return av_get_cpu_flags();
}

/* Whether the first bytes are the container they were declared: WebM's
 * EBML magic, or an MP4 box - a size and four printable letters. FFmpeg's
 * reader takes garbage for an element of unknown length and waits to skip
 * it, so a declared container is held to its first bytes here, and they
 * are put back for the reader by seeking within the buffer. */
static int declared(AVIOContext *pb, const char *container)
{
    uint8_t b[8];
    int i;

    if (avio_read(pb, b, sizeof b) != (int)sizeof b || avio_seek(pb, 0, SEEK_SET) != 0)
        return 0;
    if (!strcmp(container, "webm"))
        return b[0] == 0x1a && b[1] == 0x45 && b[2] == 0xdf && b[3] == 0xa3;
    for (i = 4; i < 8; i++) {
        if (b[i] < 0x20 || b[i] > 0x7e)
            return 0;
    }
    return 1;
}

static struct media_source *open_source(const struct media_io *io, int whole, const char *container,
                                        char *why, size_t room)
{
    const AVInputFormat *format = NULL;
    struct media_source *s;
    uint8_t *buffer;
    int r;

    av_log_set_level(AV_LOG_QUIET);
    s = calloc(1, sizeof *s);
    buffer = av_malloc(IO_BUFFER);
    if (!s || !buffer) {
        free(s);
        av_free(buffer);
        say(why, room, "media: out of memory", 0);
        return NULL;
    }
    s->io = *io;
    s->avio = avio_alloc_context(buffer, IO_BUFFER, 0, s, io_read, NULL,
                                 io->seek ? io_seek : NULL);
    s->fmt = avformat_alloc_context();
    if (!s->avio || !s->fmt) {
        if (s->avio)
            av_free(s->avio->buffer);
        else
            av_free(buffer);
        avio_context_free(&s->avio);
        avformat_free_context(s->fmt);
        free(s);
        say(why, room, "media: out of memory", 0);
        return NULL;
    }
    s->avio->seekable = io->seek ? AVIO_SEEKABLE_NORMAL : 0;
    s->fmt->pb = s->avio;
    s->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (container && !(format = av_find_input_format(container))) {
        media_close(s);
        say(why, room, "media: not a container this kit reads", 0);
        return NULL;
    }
    if (container && !declared(s->avio, container)) {
        media_close(s);
        say(why, room, "media: these bytes are not the container they were declared", 0);
        return NULL;
    }
    r = avformat_open_input(&s->fmt, NULL, format, NULL);
    if (r < 0) {
        /* `avformat_open_input` freed the context on failure. */
        av_free(s->avio->buffer);
        avio_context_free(&s->avio);
        free(s);
        say(why, room, "media: not a container this kit reads", r);
        return NULL;
    }
    r = whole ? avformat_find_stream_info(s->fmt, NULL) : 0;
    if (r < 0) {
        media_close(s);
        say(why, room, "media: its streams cannot be read", r);
        return NULL;
    }
    return s;
}

struct media_source *media_open(const struct media_io *io, char *why, size_t room)
{
    return open_source(io, 1, NULL, why, room);
}

struct media_source *media_open_appended(const struct media_io *io, const char *container,
                                         char *why, size_t room)
{
    return open_source(io, 0, container, why, room);
}

int media_stream_count(const struct media_source *s)
{
    return (int)s->fmt->nb_streams;
}

/* What `media_sound` hands back by itself for a decoder's sample format. */
static int pcm_of(int format)
{
    switch (format) {
    case AV_SAMPLE_FMT_U8:  case AV_SAMPLE_FMT_U8P:
    case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S16P:
        return MEDIA_PCM_S16;
    case AV_SAMPLE_FMT_S32: case AV_SAMPLE_FMT_S32P:
        return MEDIA_PCM_S32;
    case AV_SAMPLE_FMT_FLT: case AV_SAMPLE_FMT_FLTP:
    case AV_SAMPLE_FMT_DBL: case AV_SAMPLE_FMT_DBLP:
        return MEDIA_PCM_F32;
    default:
        return 0;
    }
}

static int is_lossless(enum AVCodecID id)
{
    const AVCodecDescriptor *desc = avcodec_descriptor_get(id);

    return desc && (desc->props & AV_CODEC_PROP_LOSSLESS) && !(desc->props & AV_CODEC_PROP_LOSSY);
}

/* A lossless source's bits a sample: what the decoder or the container
 * said (FLAC's STREAMINFO, ALAC's cookie), or what the PCM codec is. 0
 * for a lossy one. */
static int source_bits(enum AVCodecID id, int raw, int coded)
{
    int bits;

    if (!is_lossless(id))
        return 0;
    bits = raw > 0 ? raw : av_get_exact_bits_per_sample(id);
    return bits > 0 ? bits : coded > 0 ? coded : 0;
}

int media_stream(const struct media_source *s, int i, struct media_stream *out)
{
    const AVStream *st;
    const AVCodecParameters *p;
    const AVPixFmtDescriptor *d;

    if (i < 0 || (unsigned)i >= s->fmt->nb_streams)
        return -1;
    st = s->fmt->streams[i];
    p = st->codecpar;
    memset(out, 0, sizeof *out);
    out->kind = p->codec_type == AVMEDIA_TYPE_VIDEO ? MEDIA_VIDEO
              : p->codec_type == AVMEDIA_TYPE_AUDIO ? MEDIA_AUDIO : MEDIA_OTHER;
    snprintf(out->codec, sizeof out->codec, "%s", avcodec_get_name(p->codec_id));
    {
        const char *profile = avcodec_profile_name(p->codec_id, p->profile);

        snprintf(out->profile, sizeof out->profile, "%s", profile ? profile : "");
    }
    out->duration_us = to_us(st->duration, st->time_base);
    if (out->duration_us == MEDIA_NO_TIME && s->fmt->duration != AV_NOPTS_VALUE)
        out->duration_us = s->fmt->duration;
    if (out->kind == MEDIA_VIDEO) {
        out->width = p->width;
        out->height = p->height;
        d = av_pix_fmt_desc_get((enum AVPixelFormat)p->format);
        out->bits = d ? d->comp[0].depth : 8;
        if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
            out->fps_num = st->avg_frame_rate.num;
            out->fps_den = st->avg_frame_rate.den;
        }
    } else if (out->kind == MEDIA_AUDIO) {
        out->rate = p->sample_rate;
        out->channels = p->ch_layout.nb_channels;
        out->lossless = is_lossless(p->codec_id);
        out->source_bits = source_bits(p->codec_id, p->bits_per_raw_sample,
                                       p->bits_per_coded_sample);
        out->pcm = pcm_of(p->format);
        out->source_float = out->lossless && out->pcm == MEDIA_PCM_F32;
        out->bitrate = p->bit_rate > 0 ? p->bit_rate : 0;
    }
    return 0;
}

int media_read(struct media_source *s, struct media_packet *out)
{
    AVPacket *pkt = av_packet_alloc();
    AVRational tb;
    int r;

    memset(out, 0, sizeof *out);
    if (!pkt)
        return AVERROR(ENOMEM);
    r = av_read_frame(s->fmt, pkt);
    if (r == AVERROR_EOF) {
        av_packet_free(&pkt);
        return 0;
    }
    if (r < 0) {
        av_packet_free(&pkt);
        return r;
    }
    tb = s->fmt->streams[pkt->stream_index]->time_base;
    out->stream = pkt->stream_index;
    out->pts_us = to_us(pkt->pts, tb);
    out->dts_us = to_us(pkt->dts, tb);
    out->duration_us = pkt->duration ? to_us(pkt->duration, tb) : 0;
    out->key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    out->data = pkt->data;
    out->size = (size_t)pkt->size;
    out->own = pkt;
    return 1;
}

void media_packet_free(struct media_packet *p)
{
    AVPacket *pkt = p->own;

    av_packet_free(&pkt);
    memset(p, 0, sizeof *p);
}

int media_seek(struct media_source *s, int64_t us)
{
    int i = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    AVRational tb;

    if (i < 0)
        return av_seek_frame(s->fmt, -1, us, AVSEEK_FLAG_BACKWARD);
    tb = s->fmt->streams[i]->time_base;
    return av_seek_frame(s->fmt, i, from_us(us, tb), AVSEEK_FLAG_BACKWARD);
}

void media_close(struct media_source *s)
{
    if (!s)
        return;
    avformat_close_input(&s->fmt);
    if (s->avio)
        av_free(s->avio->buffer);
    avio_context_free(&s->avio);
    free(s);
}

struct media_decoder *media_decoder_open(const struct media_source *s, int i, int threads,
                                         char *why, size_t room)
{
    return media_decoder_open_with(s, i, threads, 0, why, room);
}

struct media_decoder *media_decoder_open_with(const struct media_source *s, int i, int threads,
                                              unsigned flags, char *why, size_t room)
{
    const AVStream *st;
    const AVCodec *codec;
    struct media_decoder *d;
    int r;

    if (i < 0 || (unsigned)i >= s->fmt->nb_streams) {
        say(why, room, "media: no such stream", 0);
        return NULL;
    }
    st = s->fmt->streams[i];
    codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        say(why, room, "media: no decoder for this stream", 0);
        return NULL;
    }
    d = calloc(1, sizeof *d);
    if (!d || !(d->ctx = avcodec_alloc_context3(codec)) || !(d->frame = av_frame_alloc())) {
        media_decoder_close(d);
        say(why, room, "media: out of memory", 0);
        return NULL;
    }
    r = avcodec_parameters_to_context(d->ctx, st->codecpar);
    if (r < 0) {
        media_decoder_close(d);
        say(why, room, "media: the stream's parameters", r);
        return NULL;
    }
    d->tb = st->time_base;
    d->ctx->pkt_timebase = st->time_base;
    d->ctx->thread_count = threads;
    d->ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if ((flags & MEDIA_UNTRIMMED) && st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        d->ctx->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
    d->kind = st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? MEDIA_VIDEO
            : st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ? MEDIA_AUDIO : MEDIA_OTHER;
    r = avcodec_open2(d->ctx, codec, NULL);
    if (r < 0) {
        media_decoder_close(d);
        say(why, room, "media: the decoder would not open", r);
        return NULL;
    }
    d->source_bits = source_bits(st->codecpar->codec_id,
                                 d->ctx->bits_per_raw_sample > 0 ? d->ctx->bits_per_raw_sample
                                                                 : st->codecpar->bits_per_raw_sample,
                                 st->codecpar->bits_per_coded_sample);
    return d;
}

int media_decode(struct media_decoder *d, const struct media_packet *p)
{
    AVPacket *pkt;
    int r;

    if (!p)
        return avcodec_send_packet(d->ctx, NULL);
    if (p->own)
        return avcodec_send_packet(d->ctx, p->own);
    /* A packet the caller made (MSE's samples): FFmpeg copies bytes it
     * does not own a reference to. */
    pkt = av_packet_alloc();
    if (!pkt)
        return AVERROR(ENOMEM);
    pkt->data = (uint8_t *)p->data;
    pkt->size = (int)p->size;
    pkt->pts = from_us(p->pts_us, d->tb);
    pkt->dts = from_us(p->dts_us, d->tb);
    pkt->duration = p->duration_us > 0 ? from_us(p->duration_us, d->tb) : 0;
    if (p->key)
        pkt->flags |= AV_PKT_FLAG_KEY;
    r = avcodec_send_packet(d->ctx, pkt);
    av_packet_free(&pkt);
    return r;
}

static int picture(struct media_decoder *d, struct media_frame *out)
{
    const AVFrame *f = d->frame;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((enum AVPixelFormat)f->format);
    int lines[4];

    if (!desc || av_image_fill_linesizes(lines, (enum AVPixelFormat)f->format, f->width) < 0)
        return AVERROR(EINVAL);
    out->width = f->width;
    out->height = f->height;
    out->bits = desc->comp[0].depth;
    out->planes = av_pix_fmt_count_planes((enum AVPixelFormat)f->format);
    if (out->planes > 3)
        out->planes = 3;            /* an alpha plane is not handed back */
    for (int i = 0; i < out->planes; i++) {
        int chroma = i == 1 || i == 2;
        out->plane[i] = f->data[i];
        out->stride[i] = f->linesize[i];
        out->plane_bytes[i] = lines[i];
        out->plane_rows[i] = chroma ? AV_CEIL_RSHIFT(f->height, desc->log2_chroma_h)
                                    : f->height;
    }
    if (desc->flags & AV_PIX_FMT_FLAG_RGB)
        out->format = MEDIA_FORMAT_OTHER;
    else if (out->planes == 2 && desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1)
        out->format = MEDIA_NV12;
    else if (out->planes == 3 && desc->log2_chroma_w == 1)
        out->format = desc->log2_chroma_h == 1 ? MEDIA_YUV420 : MEDIA_YUV422;
    else if (out->planes == 3 && desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0)
        out->format = MEDIA_YUV444;
    else
        out->format = MEDIA_FORMAT_OTHER;
    return 1;
}

static int sound(struct media_decoder *d, struct media_frame *out)
{
    const AVFrame *f = d->frame;
    int channels = f->ch_layout.nb_channels, room, made, r;
    uint8_t *to[1];

    if (!d->swr || d->swr_format != f->format || d->swr_rate != f->sample_rate
        || av_channel_layout_compare(&d->swr_layout, &f->ch_layout) != 0) {
        swr_free(&d->swr);
        av_channel_layout_uninit(&d->swr_layout);
        r = swr_alloc_set_opts2(&d->swr, &f->ch_layout, AV_SAMPLE_FMT_FLT, f->sample_rate,
                                &f->ch_layout, (enum AVSampleFormat)f->format,
                                f->sample_rate, 0, NULL);
        if (r < 0 || (r = swr_init(d->swr)) < 0) {
            swr_free(&d->swr);
            return r < 0 ? r : AVERROR(EINVAL);
        }
        d->swr_format = f->format;
        d->swr_rate = f->sample_rate;
        av_channel_layout_copy(&d->swr_layout, &f->ch_layout);
    }
    room = swr_get_out_samples(d->swr, f->nb_samples);
    if (room > d->pcm_room) {
        float *grown = realloc(d->pcm, (size_t)room * (size_t)channels * sizeof(float));
        if (!grown)
            return AVERROR(ENOMEM);
        d->pcm = grown;
        d->pcm_room = room;
    }
    to[0] = (uint8_t *)d->pcm;
    made = swr_convert(d->swr, to, room, (const uint8_t **)f->extended_data, f->nb_samples);
    if (made < 0)
        return made;
    out->samples = made;
    out->channels = channels;
    out->rate = f->sample_rate;
    out->pcm = d->pcm;
    return 1;
}

/* The next frame into `d->frame`: 1, 0 for another packet, -1 at the end,
 * or a fault below it. */
static int receive(struct media_decoder *d)
{
    int r;

    av_frame_unref(d->frame);
    r = avcodec_receive_frame(d->ctx, d->frame);
    if (r == AVERROR(EAGAIN))
        return 0;
    if (r == AVERROR_EOF)
        return -1;
    if (r < 0)
        return r < -1 ? r : -2;
    return 1;
}

int media_frame(struct media_decoder *d, struct media_frame *out)
{
    int r;

    memset(out, 0, sizeof *out);
    r = receive(d);
    if (r != 1)
        return r;
    out->kind = d->kind;
    out->pts_us = to_us(d->frame->best_effort_timestamp, d->tb);
    r = d->kind == MEDIA_VIDEO ? picture(d, out)
      : d->kind == MEDIA_AUDIO ? sound(d, out) : 1;
    return r < -1 || r == 1 ? r : -2;
}

static void *grow(void **buf, size_t *have, size_t want)
{
    if (*have < want) {
        void *grown = realloc(*buf, want);

        if (!grown)
            return NULL;
        *buf = grown;
        *have = want;
    }
    return *buf;
}

/* To the nearest, halves away from nought, and clipped: what narrowing
 * does, without dither (ReplayGain's is `player.md` P11's). */
static int32_t nearest(double v, double lo, double hi)
{
    v = v < lo ? lo : v > hi ? hi : v;
    return (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
}

/* The frame interleaved in its own format, MEDIA_PCM_S16, S32 or F32:
 * packed 16-bit, 32-bit and float handed back where the decoder put them,
 * planar ones interleaved, 8-bit widened and 64-bit float narrowed. */
static const void *native(struct media_decoder *d, int *format)
{
    const AVFrame *f = d->frame;
    int channels = f->ch_layout.nb_channels, n = f->nb_samples, c, i;
    int planar = av_sample_fmt_is_planar((enum AVSampleFormat)f->format);
    size_t total = (size_t)n * (size_t)channels;
    uint8_t *const *in = f->extended_data;

    *format = pcm_of(f->format);
    switch (f->format) {
    case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S32: case AV_SAMPLE_FMT_FLT:
        return in[0];
    case AV_SAMPLE_FMT_S16P: {
        int16_t *to = grow(&d->native, &d->native_room, total * sizeof *to);

        if (!to)
            return NULL;
        for (c = 0; c < channels; c++) {
            const int16_t *from = (const int16_t *)(const void *)in[c];

            for (i = 0; i < n; i++)
                to[(size_t)i * channels + c] = from[i];
        }
        return to;
    }
    case AV_SAMPLE_FMT_S32P: case AV_SAMPLE_FMT_FLTP: {
        uint32_t *to = grow(&d->native, &d->native_room, total * sizeof *to);

        if (!to)
            return NULL;
        for (c = 0; c < channels; c++) {
            const uint32_t *from = (const uint32_t *)(const void *)in[c];

            for (i = 0; i < n; i++)
                to[(size_t)i * channels + c] = from[i];
        }
        return to;
    }
    case AV_SAMPLE_FMT_U8: case AV_SAMPLE_FMT_U8P: {
        int16_t *to = grow(&d->native, &d->native_room, total * sizeof *to);

        if (!to)
            return NULL;
        for (c = 0; c < (planar ? channels : 1); c++) {
            const uint8_t *from = in[c];
            size_t many = planar ? (size_t)n : total, k;

            for (k = 0; k < many; k++) {
                size_t at = planar ? k * (size_t)channels + (size_t)c : k;

                to[at] = (int16_t)(((int)from[k] - 128) * 256);
            }
        }
        return to;
    }
    case AV_SAMPLE_FMT_DBL: case AV_SAMPLE_FMT_DBLP: {
        float *to = grow(&d->native, &d->native_room, total * sizeof *to);

        if (!to)
            return NULL;
        for (c = 0; c < (planar ? channels : 1); c++) {
            const double *from = (const double *)(const void *)in[c];
            size_t many = planar ? (size_t)n : total, k;

            for (k = 0; k < many; k++)
                to[planar ? k * (size_t)channels + (size_t)c : k] = (float)from[k];
        }
        return to;
    }
    default:
        return NULL;
    }
}

/* `from`, `total` samples in `have`, converted to `want`. */
static const void *convert(struct media_decoder *d, const void *from, int have, int want,
                           size_t total)
{
    size_t size = want == MEDIA_PCM_S16 ? sizeof(int16_t) : 4, k;
    void *to = grow(&d->asked, &d->asked_room, total * size);
    const int16_t *s16 = from;
    const int32_t *s32 = from;
    const float *f32 = from;
    int16_t *t16 = to;
    int32_t *t32 = to;
    float *tf = to;

    if (!to)
        return NULL;
    for (k = 0; k < total; k++) {
        if (have == MEDIA_PCM_S16 && want == MEDIA_PCM_S32)
            t32[k] = (int32_t)((uint32_t)(int32_t)s16[k] << 16);
        else if (have == MEDIA_PCM_S16)
            tf[k] = (float)s16[k] / 32768.0f;
        else if (have == MEDIA_PCM_S32 && want == MEDIA_PCM_S16)
            t16[k] = (int16_t)nearest((double)s32[k] / 65536.0, -32768.0, 32767.0);
        else if (have == MEDIA_PCM_S32)
            tf[k] = (float)((double)s32[k] / 2147483648.0);
        else if (want == MEDIA_PCM_S16)
            t16[k] = (int16_t)nearest((double)f32[k] * 32768.0, -32768.0, 32767.0);
        else
            t32[k] = nearest((double)f32[k] * 2147483648.0, -2147483648.0, 2147483647.0);
    }
    return to;
}

int media_sound(struct media_decoder *d, int want, struct media_sound *out)
{
    const void *pcm;
    size_t total;
    int r, have;

    memset(out, 0, sizeof *out);
    if (d->kind != MEDIA_AUDIO || want < MEDIA_PCM_NATIVE || want > MEDIA_PCM_F32)
        return AVERROR(EINVAL);
    r = receive(d);
    if (r != 1)
        return r;
    total = (size_t)d->frame->nb_samples * (size_t)d->frame->ch_layout.nb_channels;
    pcm = native(d, &have);
    if (!have)
        return AVERROR(ENOSYS);         /* a sample format no codec here makes */
    if (!pcm)
        return AVERROR(ENOMEM);
    if (want != MEDIA_PCM_NATIVE && want != have) {
        pcm = convert(d, pcm, have, want, total);
        if (!pcm)
            return AVERROR(ENOMEM);
        have = want;
    }
    out->pts_us = to_us(d->frame->best_effort_timestamp, d->tb);
    out->samples = d->frame->nb_samples;
    out->channels = d->frame->ch_layout.nb_channels;
    out->rate = d->frame->sample_rate;
    out->format = have;
    out->bits = d->source_bits;
    out->pcm = pcm;
    out->bytes = total * (have == MEDIA_PCM_S16 ? sizeof(int16_t) : 4);
    return 1;
}

void media_decoder_flush(struct media_decoder *d)
{
    avcodec_flush_buffers(d->ctx);
    if (d->swr) {
        swr_free(&d->swr);
        av_channel_layout_uninit(&d->swr_layout);
    }
}

void media_decoder_close(struct media_decoder *d)
{
    if (!d)
        return;
    av_frame_free(&d->frame);
    avcodec_free_context(&d->ctx);
    swr_free(&d->swr);
    av_channel_layout_uninit(&d->swr_layout);
    free(d->pcm);
    free(d->native);
    free(d->asked);
    free(d);
}

const char *media_error(int code, char *buf, size_t room)
{
    if (av_strerror(code, buf, room) < 0)
        snprintf(buf, room, "media: fault %d", code);
    return buf;
}
