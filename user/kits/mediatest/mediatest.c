/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Media Kit, held to this Mac** (`docs/media.md` M1,
 * `tools/run_mediakit.py`): every file `FOLDER/manifest.txt` names, opened
 * through the kit, every stream decoded to its end on two threads, and
 * then a seek to the middle - one line said for each of those. Every line
 * is what the same file says compiled on this Mac against the same FFmpeg
 * (`tools/w5_build.py host`), and a picture's fingerprint is also what
 * FFmpeg's own `ffmpeg -f framemd5` says of it, so the kit is held to a
 * reference it did not make, twice.
 *
 * - **A picture stream**: its frames, an FNV-1a over their timestamps, and
 *   the MD5 of the concatenated MD5s of each frame's planes packed - the
 *   very digest `framemd5` gives a frame, which is why it is MD5.
 * - **A sound stream**: its samples, and the MD5 of the interleaved float
 *   the kit hands back.
 * - **The seek**: where the key picture before the middle is, and its MD5.
 *
 * The processor's extensions are narrowed to a set both machines have
 * (`media_cpu_flags`) and said in the first line: float decoders round
 * differently down different SIMD paths, which the pictures never do.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/cpu.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>

#include "media.h"
#include "mediatest.h"

static mediatest_say_fn say_fn;
static void *say_ctx;

static void say(const char *format, ...)
{
    char line[1024];
    va_list ap;

    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    if (say_fn)
        say_fn(line, say_ctx);
    else
        puts(line);
}

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
    const uint8_t *p = data;

    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

#define FNV_START 0xcbf29ce484222325ull

static void hex(const uint8_t sum[16], char out[33])
{
    for (int i = 0; i < 16; i++)
        snprintf(out + 2 * i, 3, "%02x", sum[i]);
}

/* A frame's planes packed, row by row, as `av_image_copy_to_buffer`
 * packs them for `framemd5`. */
static void picture_md5(const struct media_frame *f, char out[33])
{
    struct AVMD5 *m = av_md5_alloc();
    uint8_t sum[16];

    av_md5_init(m);
    for (int i = 0; i < f->planes; i++)
        for (int y = 0; y < f->plane_rows[i]; y++)
            av_md5_update(m, f->plane[i] + (size_t)y * (size_t)f->stride[i],
                          (size_t)f->plane_bytes[i]);
    av_md5_final(m, sum);
    av_free(m);
    hex(sum, out);
}

struct file {
    FILE *f;
};

static long file_read(void *ctx, uint8_t *buf, size_t n)
{
    struct file *file = ctx;
    size_t got = fread(buf, 1, n, file->f);

    if (got == 0 && ferror(file->f))
        return -1;
    return (long)got;
}

static int64_t file_seek(void *ctx, int64_t offset, int whence)
{
    struct file *file = ctx;

    if (fseek(file->f, (long)offset, whence) != 0)
        return -1;
    return ftell(file->f);
}

static const char *format_name(int format)
{
    switch (format) {
    case MEDIA_YUV420: return "yuv420";
    case MEDIA_YUV422: return "yuv422";
    case MEDIA_YUV444: return "yuv444";
    case MEDIA_NV12: return "nv12";
    default: return "other";
    }
}

#define MAX_STREAMS 8

struct track {
    struct media_decoder *dec;
    struct media_stream info;
    int frames, format;
    long long samples;
    uint64_t times;
    struct AVMD5 *all;          /* of the frames' MD5s, or of the samples */
};

/* What a decoder has ready, into its track. */
static int drain(struct track *t)
{
    struct media_frame f;
    char sum[33];
    int r;

    while ((r = media_frame(t->dec, &f)) == 1) {
        t->frames++;
        t->times = fnv(t->times, &f.pts_us, sizeof f.pts_us);
        if (f.kind == MEDIA_VIDEO) {
            picture_md5(&f, sum);
            av_md5_update(t->all, (const uint8_t *)sum, 32);
            t->format = f.format;
        } else if (f.kind == MEDIA_AUDIO) {
            t->samples += f.samples;
            av_md5_update(t->all, (const uint8_t *)f.pcm,
                          (size_t)f.samples * (size_t)f.channels * sizeof(float));
        }
    }
    return r;
}

static void one(const char *folder, const char *name)
{
    char path[1024], why[256], sum[33], e[128];
    struct track tracks[MAX_STREAMS];
    struct media_source *src;
    struct media_packet p;
    struct file file;
    struct media_io io;
    uint8_t digest[16];
    int n, r, middle_stream = -1;
    int64_t longest = 0;

    snprintf(path, sizeof path, "%s/%s", folder, name);
    file.f = fopen(path, "rb");
    if (!file.f) {
        say("file %s: not there", name);
        return;
    }
    fseek(file.f, 0, SEEK_END);
    io.ctx = &file;
    io.read = file_read;
    io.seek = file_seek;
    io.size = ftell(file.f);
    fseek(file.f, 0, SEEK_SET);

    src = media_open(&io, why, sizeof why);
    if (!src) {
        say("file %s: %s", name, why);
        fclose(file.f);
        return;
    }
    n = media_stream_count(src);
    if (n > MAX_STREAMS)
        n = MAX_STREAMS;
    say("file %s: %d stream%s", name, n, n == 1 ? "" : "s");
    memset(tracks, 0, sizeof tracks);
    for (int i = 0; i < n; i++) {
        struct track *t = &tracks[i];

        media_stream(src, i, &t->info);
        t->times = FNV_START;
        if (t->info.kind == MEDIA_OTHER)
            continue;
        t->dec = media_decoder_open(src, i, 2, why, sizeof why);
        if (!t->dec) {
            say("stream %s #%d %s: %s", name, i, t->info.codec, why);
            continue;
        }
        t->all = av_md5_alloc();
        av_md5_init(t->all);
        if (t->info.kind == MEDIA_VIDEO && middle_stream < 0)
            middle_stream = i;
        if (t->info.duration_us != MEDIA_NO_TIME && t->info.duration_us > longest)
            longest = t->info.duration_us;
    }

    while ((r = media_read(src, &p)) == 1) {
        struct track *t = p.stream < n ? &tracks[p.stream] : NULL;

        if (t && t->dec) {
            r = media_decode(t->dec, &p);
            if (r < 0 && r != -11 /* EAGAIN: the frames first */)
                say("stream %s #%d: a packet refused: %s", name, p.stream,
                    media_error(r, e, sizeof e));
            drain(t);
        }
        media_packet_free(&p);
    }
    if (r < 0)
        say("file %s: read stopped: %s", name, media_error(r, e, sizeof e));

    for (int i = 0; i < n; i++) {
        struct track *t = &tracks[i];

        if (!t->dec)
            continue;
        media_decode(t->dec, NULL);
        r = drain(t);
        av_md5_final(t->all, digest);
        av_free(t->all);
        hex(digest, sum);
        if (t->info.kind == MEDIA_VIDEO)
            say("stream %s #%d video %s %dx%d %d bits %s: %d frames, times %016llx, %s%s",
                name, i, t->info.codec, t->info.width, t->info.height, t->info.bits,
                format_name(t->format), t->frames, (unsigned long long)t->times, sum,
                r < -1 ? " (a fault at the end)" : "");
        else
            say("stream %s #%d audio %s %d Hz %d ch: %lld samples, times %016llx, %s%s",
                name, i, t->info.codec, t->info.rate, t->info.channels, t->samples,
                (unsigned long long)t->times, sum, r < -1 ? " (a fault at the end)" : "");
    }

    /* The seek: to the middle, then the first picture that comes out. */
    if (middle_stream >= 0 && longest > 0) {
        struct track *t = &tracks[middle_stream];
        struct media_frame f;
        int got = 0;

        r = media_seek(src, longest / 2);
        media_decoder_flush(t->dec);
        while (r >= 0 && !got && media_read(src, &p) == 1) {
            if (p.stream == middle_stream) {
                media_decode(t->dec, &p);
                if (media_frame(t->dec, &f) == 1)
                    got = 1;
            }
            media_packet_free(&p);
        }
        if (r >= 0 && !got) {
            media_decode(t->dec, NULL);
            got = media_frame(t->dec, &f) == 1;
        }
        if (got) {
            picture_md5(&f, sum);
            say("seek %s %lld ms: a picture at %lld ms, %s", name,
                (long long)(longest / 2000), (long long)(f.pts_us / 1000), sum);
        } else {
            say("seek %s %lld ms: %s", name, (long long)(longest / 2000),
                r < 0 ? media_error(r, e, sizeof e) : "no picture after it");
        }
    }

    for (int i = 0; i < n; i++)
        media_decoder_close(tracks[i].dec);
    media_close(src);
    fclose(file.f);
}

int mediatest_run(const char *folder, mediatest_say_fn fn, void *ctx)
{
    char path[1024], line[512];
    FILE *manifest;
    int files = 0, flags;

#if defined(__x86_64__)
    flags = media_cpu_flags(AV_CPU_FLAG_MMX | AV_CPU_FLAG_MMXEXT | AV_CPU_FLAG_SSE
                            | AV_CPU_FLAG_SSE2 | AV_CPU_FLAG_SSE3);
#elif defined(__aarch64__)
    flags = media_cpu_flags(AV_CPU_FLAG_ARMV8 | AV_CPU_FLAG_NEON);
#else
    flags = media_cpu_flags(-1);
#endif
    say_fn = fn;
    say_ctx = ctx;
    say("versions: FFmpeg %s, processor flags %#x", media_version(), flags);

    snprintf(path, sizeof path, "%s/manifest.txt", folder);
    manifest = fopen(path, "r");
    if (!manifest) {
        say("mediatest: no manifest in %s", folder);
        return -1;
    }
    while (fgets(line, sizeof line, manifest)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0])
            continue;
        one(folder, line);
        files++;
    }
    fclose(manifest);
    say("mediatest: %d files", files);
    return 0;
}

#ifdef MEDIATEST_HOST
int main(int argc, char **argv)
{
    return argc == 2 ? mediatest_run(argv[1], NULL, NULL) : 2;
}
#endif
