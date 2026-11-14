/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_MEDIA_H
#define KOSMOS_MEDIA_H

/*
 * **The Media Kit** (`docs/media.md` M1): a film or a song taken apart and
 * decoded - WebM, MP4 (fragmented too, as MSE's are), Ogg, MP3, FLAC, ADTS
 * and WAV; AV1 (dav1d), VP9, VP8 and H.264; Opus, AAC, Vorbis, MP3 and
 * FLAC. One door for every application that plays something: JetSki's
 * `<video>` first, the Video app and Music after it (M6). FFmpeg 9.0.2
 * built whole, with its assembly (`tools/w5_build.py`), is what does the
 * work; nothing of FFmpeg's shows through this header, so a caller never
 * includes it and a later decoder in the GPU changes no caller.
 *
 * **Bytes come from the caller**, through `struct media_io`: a file, a
 * download as it arrives, or what a page appended to a SourceBuffer - the
 * kit opens nothing itself, so it reaches nothing it was not handed.
 *
 * **Time is microseconds** everywhere, whatever the container counts in,
 * so a number that leaves this kit says what it is (`CLAUDE.md`, *Two
 * clocks*); `MEDIA_NO_TIME` where the file says none.
 *
 * A picture is handed back as the decoder made it - its planes, their
 * strides, its format - and turned into a page's pixels by whoever paints
 * it; sound is handed back as 32-bit float, interleaved, at the stream's
 * own rate, which is what the Playback Kit mixes.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_NO_TIME INT64_MIN

/* Where the bytes are. `read` fills up to `n` bytes and says how many, 0 at
 * the end and negative on failure; `seek` goes to `offset` from `whence`
 * (0 the start, 1 here, 2 the end) and says where it is, or negative when
 * it cannot - a stream that cannot seek leaves it NULL. `size` is the
 * whole length, or negative when nobody knows yet. */
struct media_io {
    void *ctx;
    long (*read)(void *ctx, uint8_t *buf, size_t n);
    int64_t (*seek)(void *ctx, int64_t offset, int whence);
    int64_t size;
};

enum media_kind { MEDIA_OTHER = 0, MEDIA_VIDEO = 1, MEDIA_AUDIO = 2 };

/* A picture's layout: 4:2:0, 4:2:2 or 4:4:4, at 8 bits or more. Anything
 * else a decoder could make is MEDIA_FORMAT_OTHER, and still hashed and
 * handed back plane by plane. */
enum media_format {
    MEDIA_FORMAT_OTHER = 0,
    MEDIA_YUV420 = 1, MEDIA_YUV422 = 2, MEDIA_YUV444 = 3,
    MEDIA_NV12 = 4,
};

struct media_stream {
    int kind;                   /* enum media_kind */
    char codec[16];             /* "av1", "vp9", "h264", "opus", "aac", ... */
    int64_t duration_us;        /* MEDIA_NO_TIME when the file does not say */
    /* a picture */
    int width, height;
    int bits;                   /* 8, 10, 12 */
    int fps_num, fps_den;       /* 0 when the file does not say */
    /* a sound */
    int rate, channels;
};

struct media_packet {
    int stream;
    int64_t pts_us, dts_us, duration_us;
    int key;                    /* a picture that needs no other to decode */
    const uint8_t *data;
    size_t size;
    void *own;                  /* the kit's; `media_packet_free` */
};

struct media_frame {
    int kind;                   /* enum media_kind */
    int64_t pts_us;
    /* a picture: up to three planes, each `plane_bytes[i]` wide in bytes
     * and `plane_rows[i]` tall, `stride[i]` apart */
    int width, height, bits, format;
    int planes;
    const uint8_t *plane[3];
    int stride[3], plane_bytes[3], plane_rows[3];
    /* a sound: `samples` a channel, interleaved float */
    int samples, channels, rate;
    const float *pcm;
};

struct media_source;
struct media_decoder;

/* The kit's FFmpeg, and the processors' extensions it decodes with. */
const char *media_version(void);

/* A container read from `io`, which must outlive it. NULL, and why in
 * `why`, when it is not one this kit reads. */
struct media_source *media_open(const struct media_io *io, char *why, size_t room);
/* The same, for bytes that arrive in pieces - MSE's appends: the header
 * read and nothing past it, where `media_open` reads on until it knows
 * every stream's every parameter, which is seconds of film. What the
 * header leaves out a decoder learns from the first packets. `io` reads
 * forward only, and waits rather than answering 0 while more may come.
 * `container` is the one the bytes were declared to be - "webm", "mp4" -
 * since guessing from bytes that have not arrived would wait for them:
 * bytes that are not one fail at once. NULL guesses. */
struct media_source *media_open_appended(const struct media_io *io, const char *container,
                                         char *why, size_t room);
int media_stream_count(const struct media_source *src);
int media_stream(const struct media_source *src, int i, struct media_stream *out);
/* The next packet of any stream: 1 a packet, 0 the end, negative a fault
 * (`media_error`). */
int media_read(struct media_source *src, struct media_packet *out);
void media_packet_free(struct media_packet *p);
/* To the key picture at or before `us` in the first picture stream. */
int media_seek(struct media_source *src, int64_t us);
void media_close(struct media_source *src);

/* A decoder for stream `i`, on `threads` threads (0: as many as there are
 * processors). */
struct media_decoder *media_decoder_open(const struct media_source *src, int i, int threads,
                                         char *why, size_t room);
/* A packet in, or NULL at the end to have the decoder give up what it
 * holds. 0, or negative. */
int media_decode(struct media_decoder *d, const struct media_packet *p);
/* A frame out: 1 a frame, valid until the next call; 0 when it wants
 * another packet; -1 when everything has come out; below -1 a fault. */
int media_frame(struct media_decoder *d, struct media_frame *out);
void media_decoder_flush(struct media_decoder *d);
void media_decoder_close(struct media_decoder *d);

/* The sentence for a negative answer. */
const char *media_error(int code, char *buf, size_t room);

/* For a test that holds two machines to each other: decode with only
 * these of the processor's extensions (FFmpeg's flags), or every one it
 * has with -1. Returns what is now in use. */
int media_cpu_flags(int mask);

#ifdef __cplusplus
}
#endif

#endif
