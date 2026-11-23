/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Media Kit's sound at the file's own resolution, on the Mac**
 * (`docs/player.md` P1): `media_sound` against this Mac's own FFmpeg.
 *
 * The files are made here, at the start, by the Mac's `ffmpeg` from a
 * 997 Hz sine, a sweep and noise from fixed seeds (`aevalsrc`'s
 * `random`), each channel different from the other - no one's music - in
 * every container and depth Music opens:
 * WAV (8, 16, 24 and 32-bit integer, 32 and 64-bit float), AIFF (16, 24,
 * 32-bit, float), FLAC (16 and 24-bit), ALAC in an M4A (16 and 24-bit) and
 * MP3, at 44.1, 48, 96 and 192 kHz.
 *
 * **Each is decoded by the kit and its samples' MD5 held to the Mac's
 * `ffmpeg -c:a pcm_<format> -f md5`** of the same file, in the format the
 * kit says is its own - s16, s32 or f32. The Mac's `ffmpeg` is a build of
 * its own (Homebrew's), so the two agree only if the kit hands back what
 * the decoder made, every sample, in order. A FLAC is held as well to the
 * MD5 its STREAMINFO carries, which is the encoder's own word on what the
 * samples are, at the source's depth. And:
 *
 *   - `media_stream` says each one's codec, rate, channels, source depth,
 *     lossless or not, float or not, and the native format (an MP3's is
 *     s16: FFmpeg's fixed-point `mp3` is the decoder built);
 *   - a 24-bit source asked for as f32 and turned back into s32 is exact
 *     (float's mantissa is 24 bits), and a 32-bit one is not - which is
 *     why music is not handed over as float;
 *   - a 16-bit source asked for as s32 is ffmpeg's s32 of it, exactly.
 *
 * **Control**: MEDIASOUNDTEST_BROKEN=1 takes every s32 source through
 * float - asks for f32 and turns it back into s32 - before the MD5, as a
 * kit that converted to float would; the 32-bit WAV and AIFF of noise
 * must then fail, and the test exits 1.
 *
 * It also says what a 96 kHz / 24-bit FLAC costs to decode, a second of
 * sound, on this Mac's core (a QEMU number would not be one).
 *
 * Usage: test_mediasound <folder for the files it makes>
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include <libavutil/mem.h>
#include <libavutil/md5.h>

#include "media.h"

static int checks, fails, broken;
/* Decode, and ask the reference for, the sound untrimmed (`MEDIA_UNTRIMMED`). */
static int untrim;

#define CHECK(ok, ...) do { checks++; if (!(ok)) { fails++; \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *dir;

/* ---- the files, made by the Mac's ffmpeg ---- */

enum { SINE, SWEEP, NOISE };

struct file {
    const char *name;           /* in `dir` */
    int signal, rate, channels;
    const char *args;           /* ffmpeg's, for the codec and depth */
    /* what the kit must say */
    const char *codec;
    int pcm;                    /* MEDIA_PCM_* */
    int bits, lossless, is_float;
    double seconds;
};

static const struct file files[] = {
    { "wav-u8-44100.wav",    SINE,  44100, 1, "-c:a pcm_u8",    "pcm_u8",    MEDIA_PCM_S16, 8,  1, 0, 2 },
    { "wav-s16-44100.wav",   SINE,  44100, 2, "-c:a pcm_s16le", "pcm_s16le", MEDIA_PCM_S16, 16, 1, 0, 2 },
    { "wav-s16-48000.wav",   NOISE, 48000, 2, "-c:a pcm_s16le", "pcm_s16le", MEDIA_PCM_S16, 16, 1, 0, 2 },
    { "wav-s24-96000.wav",   NOISE, 96000, 2, "-c:a pcm_s24le", "pcm_s24le", MEDIA_PCM_S32, 24, 1, 0, 2 },
    { "wav-s24-192000.wav",  SWEEP, 192000, 2, "-c:a pcm_s24le", "pcm_s24le", MEDIA_PCM_S32, 24, 1, 0, 2 },
    { "wav-s32-48000.wav",   NOISE, 48000, 2, "-c:a pcm_s32le", "pcm_s32le", MEDIA_PCM_S32, 32, 1, 0, 2 },
    { "wav-s32-192000.wav",  NOISE, 192000, 2, "-c:a pcm_s32le", "pcm_s32le", MEDIA_PCM_S32, 32, 1, 0, 1 },
    { "wav-f32-48000.wav",   SWEEP, 48000, 2, "-c:a pcm_f32le", "pcm_f32le", MEDIA_PCM_F32, 32, 1, 1, 2 },
    { "wav-f64-44100.wav",   SWEEP, 44100, 2, "-c:a pcm_f64le", "pcm_f64le", MEDIA_PCM_F32, 64, 1, 1, 2 },
    { "aiff-s8-44100.aiff",  SINE,  44100, 1, "-c:a pcm_s8",    "pcm_s8",    MEDIA_PCM_S16, 8,  1, 0, 2 },
    { "aiff-s16-44100.aiff", SWEEP, 44100, 2, "-c:a pcm_s16be", "pcm_s16be", MEDIA_PCM_S16, 16, 1, 0, 2 },
    { "aiff-s24-96000.aiff", NOISE, 96000, 2, "-c:a pcm_s24be", "pcm_s24be", MEDIA_PCM_S32, 24, 1, 0, 2 },
    { "aiff-s32-48000.aiff", NOISE, 48000, 2, "-c:a pcm_s32be", "pcm_s32be", MEDIA_PCM_S32, 32, 1, 0, 2 },
    { "aiff-f32-48000.aiff", SWEEP, 48000, 2, "-c:a pcm_f32be", "pcm_f32be", MEDIA_PCM_F32, 32, 1, 1, 2 },
    { "aiff-f64-96000.aiff", SINE,  96000, 2, "-c:a pcm_f64be", "pcm_f64be", MEDIA_PCM_F32, 64, 1, 1, 2 },
    { "flac-16-44100.flac",  SINE,  44100, 2, "-c:a flac -sample_fmt s16", "flac", MEDIA_PCM_S16, 16, 1, 0, 2 },
    { "flac-16-48000.flac",  NOISE, 48000, 2, "-c:a flac -sample_fmt s16", "flac", MEDIA_PCM_S16, 16, 1, 0, 2 },
    { "flac-24-48000.flac",  SWEEP, 48000, 2,
      "-c:a flac -sample_fmt s32 -bits_per_raw_sample 24", "flac", MEDIA_PCM_S32, 24, 1, 0, 2 },
    { "flac-24-96000.flac",  NOISE, 96000, 2,
      "-c:a flac -sample_fmt s32 -bits_per_raw_sample 24", "flac", MEDIA_PCM_S32, 24, 1, 0, 2 },
    { "flac-24-96000-sweep.flac", SWEEP, 96000, 2,
      "-c:a flac -sample_fmt s32 -bits_per_raw_sample 24", "flac", MEDIA_PCM_S32, 24, 1, 0, 10 },
    { "flac-24-192000.flac", SWEEP, 192000, 2,
      "-c:a flac -sample_fmt s32 -bits_per_raw_sample 24", "flac", MEDIA_PCM_S32, 24, 1, 0, 2 },
    { "alac-16-44100.m4a",   SINE,  44100, 2, "-c:a alac -sample_fmt s16p", "alac", MEDIA_PCM_S16, 16, 1, 0, 2 },
    { "alac-24-96000.m4a",   NOISE, 96000, 2, "-c:a alac -sample_fmt s32p", "alac", MEDIA_PCM_S32, 24, 1, 0, 2 },
    { "alac-24-192000.m4a",  SWEEP, 192000, 2, "-c:a alac -sample_fmt s32p", "alac", MEDIA_PCM_S32, 24, 1, 0, 2 },
    { "mp3-44100.mp3",       SWEEP, 44100, 2, "-c:a libmp3lame -b:a 320k", "mp3", MEDIA_PCM_S16, 0, 0, 0, 2 },
    { "mp3-48000.mp3",       SINE,  48000, 2, "-c:a libmp3lame -b:a 192k", "mp3", MEDIA_PCM_S16, 0, 0, 0, 2 },
};

#define NFILES (sizeof files / sizeof files[0])

static void path_of(const struct file *f, char *out, size_t room)
{
    snprintf(out, room, "%s/%s", dir, f->name);
}

/* The signal, as an `aevalsrc` expression: a 997 Hz sine at -6 dB, a sweep
 * from 20 Hz to 20 kHz across the file, or noise from a fixed seed, each
 * channel its own. Made in double, so a 32-bit file's low bits are used. */
static int make(const struct file *f)
{
    char cmd[1024], path[512], expr[400];
    double d = f->seconds;

    path_of(f, path, sizeof path);
    switch (f->signal) {
    case SINE:
        snprintf(expr, sizeof expr, "0.5*sin(2*PI*997*t)%s",
                 f->channels == 2 ? "|0.5*sin(2*PI*997*t+1)" : "");
        break;
    case SWEEP:
        if (f->channels == 2)
            snprintf(expr, sizeof expr, "0.7*sin(2*PI*(20*t+(20000-20)/(2*%g)*t*t))"
                     "|0.7*cos(2*PI*(20*t+(20000-20)/(2*%g)*t*t))", d, d);
        else
            snprintf(expr, sizeof expr, "0.7*sin(2*PI*(20*t+(20000-20)/(2*%g)*t*t))", d);
        break;
    default:
        /* Each channel's `random` starts from 0, so the second is seeded
         * apart, or both would be the same and a swap would not show. */
        snprintf(expr, sizeof expr, "0.9*(2*random(0)-1)%s",
                 f->channels == 2 ? "|st(1,if(eq(n,0),987654321,ld(1)));0.9*(2*random(1)-1)" : "");
        break;
    }
    snprintf(cmd, sizeof cmd,
             "ffmpeg -nostdin -hide_banner -v error -y -f lavfi "
             "-i \"aevalsrc=exprs='%s':s=%d:d=%g\" %s -map_metadata -1 \"%s\"",
             expr, f->rate, d, f->args, path);
    return system(cmd) == 0;
}

/* The Mac's ffmpeg's MD5 of the decoded samples, in `pcm`. An MP3 is
 * decoded by FFmpeg's fixed-point `mp3`, the one `w5_build.py` builds,
 * which makes 16-bit - Homebrew's would choose `mp3float`. */
static int reference(const char *path, const char *codec_in, int pcm, char out[33])
{
    char cmd[1024], line[256];
    const char *codec = pcm == MEDIA_PCM_S16 ? "pcm_s16le" : pcm == MEDIA_PCM_S32 ? "pcm_s32le"
                                                                                   : "pcm_f32le";
    FILE *p;
    int ok = 0;

    snprintf(cmd, sizeof cmd, "ffmpeg -nostdin -hide_banner -v error %s %s -i \"%s\" -map 0:a:0 "
             "-c:a %s -f md5 -", untrim ? "-flags2 +skip_manual" : "",
             strcmp(codec_in, "mp3") ? "" : "-c:a mp3", path, codec);
    p = popen(cmd, "r");
    if (!p)
        return 0;
    while (fgets(line, sizeof line, p)) {
        if (!strncmp(line, "MD5=", 4) && strlen(line) >= 36) {
            memcpy(out, line + 4, 32);
            out[32] = 0;
            ok = 1;
        }
    }
    return pclose(p) == 0 && ok;
}

/* The MD5 a FLAC's STREAMINFO carries, or 0 when the encoder left it 0. */
static int streaminfo_md5(const char *path, char out[33])
{
    uint8_t b[42];
    FILE *f = fopen(path, "rb");
    int i, any = 0;

    if (!f)
        return 0;
    if (fread(b, 1, sizeof b, f) != sizeof b || memcmp(b, "fLaC", 4) || (b[4] & 0x7f) != 0) {
        fclose(f);
        return 0;
    }
    fclose(f);
    for (i = 0; i < 16; i++) {
        snprintf(out + 2 * i, 3, "%02x", b[8 + 18 + i]);
        any |= b[8 + 18 + i];
    }
    return any;
}

/* ---- the kit over a file ---- */

static long file_read(void *ctx, uint8_t *buf, size_t n)
{
    size_t got = fread(buf, 1, n, ctx);

    return got ? (long)got : ferror((FILE *)ctx) ? -1 : 0;
}

static int64_t file_seek(void *ctx, int64_t offset, int whence)
{
    if (fseeko(ctx, offset, whence) != 0)
        return -1;
    return ftello(ctx);
}

static void hex(const uint8_t d[16], char out[33])
{
    for (int i = 0; i < 16; i++)
        snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static int32_t nearest32(double v)
{
    v = v < -2147483648.0 ? -2147483648.0 : v > 2147483647.0 ? 2147483647.0 : v;
    return (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
}

struct decoded {
    char md5[33];               /* in the format asked for */
    char source_md5[33];        /* at the source's depth, as FLAC's STREAMINFO */
    int format, bits, rate, channels;
    long frames;
    double seconds;             /* to decode */
};

/* Every sound of the first audio stream, asked for in `want`. With
 * `through_float`, an s32 sound is asked for as f32 and turned back,
 * which must lose a 32-bit source's low bits. */
static int decode(const char *path, int want, int through_float, struct media_stream *info,
                  struct decoded *out)
{
    struct media_io io;
    struct media_source *src;
    struct media_decoder *dec;
    struct media_packet pkt;
    struct media_sound s;
    struct AVMD5 *md5 = av_md5_alloc(), *smd5 = av_md5_alloc();
    uint8_t digest[16];
    int32_t *back = NULL;
    size_t back_room = 0;
    char why[200] = { 0 };
    FILE *f = fopen(path, "rb");
    struct stat st;
    int stream = -1, r, i, end = 0, ok = 1;
    clock_t began;

    memset(out, 0, sizeof *out);
    if (!f || !md5 || !smd5 || stat(path, &st) != 0)
        return 0;
    io.ctx = f;
    io.read = file_read;
    io.seek = file_seek;
    io.size = st.st_size;
    src = media_open(&io, why, sizeof why);
    if (!src) {
        printf("  %s: %s\n", path, why);
        fclose(f);
        return 0;
    }
    for (i = 0; i < media_stream_count(src); i++) {
        if (media_stream(src, i, info) == 0 && info->kind == MEDIA_AUDIO) {
            stream = i;
            break;
        }
    }
    dec = stream >= 0 ? media_decoder_open_with(src, stream, 1, untrim ? MEDIA_UNTRIMMED : 0,
                                                 why, sizeof why) : NULL;
    if (!dec) {
        printf("  %s: %s\n", path, stream < 0 ? "no sound" : why);
        media_close(src);
        fclose(f);
        return 0;
    }
    av_md5_init(md5);
    av_md5_init(smd5);
    began = clock();
    while (ok) {
        if (!end) {
            r = media_read(src, &pkt);
            if (r < 0) {
                ok = 0;
                break;
            }
            if (r == 0) {
                end = 1;
                media_decode(dec, NULL);
            } else if (pkt.stream != stream) {
                media_packet_free(&pkt);
                continue;
            } else {
                r = media_decode(dec, &pkt);
                media_packet_free(&pkt);
                if (r < 0) {
                    ok = 0;
                    break;
                }
            }
        }
        for (;;) {
            int asked = through_float ? MEDIA_PCM_F32 : want;
            const void *pcm;
            size_t bytes, k, n;

            r = media_sound(dec, asked, &s);
            if (r == 0)
                break;
            if (r == -1) {
                goto done;
            }
            if (r < 0) {
                ok = 0;
                goto done;
            }
            pcm = s.pcm;
            bytes = s.bytes;
            n = (size_t)s.samples * (size_t)s.channels;
            if (through_float) {
                /* What a kit that converted to float would hand back. */
                if (back_room < n) {
                    back = realloc(back, n * sizeof *back);
                    back_room = n;
                }
                for (k = 0; k < n; k++)
                    back[k] = nearest32((double)((const float *)s.pcm)[k] * 2147483648.0);
                pcm = back;
                bytes = n * sizeof *back;
                s.format = MEDIA_PCM_S32;
            }
            av_md5_update(md5, pcm, bytes);
            /* At the source's depth, little-endian, as FLAC hashes it. */
            if (s.bits > 0 && s.bits <= 32 && s.format != MEDIA_PCM_F32) {
                int width = (s.bits + 7) / 8;
                int shift = (s.format == MEDIA_PCM_S16 ? 16 : 32) - s.bits;

                for (k = 0; k < n; k++) {
                    int32_t v = s.format == MEDIA_PCM_S16 ? ((const int16_t *)pcm)[k]
                                                          : ((const int32_t *)pcm)[k];
                    uint8_t le[4];

                    v >>= shift;
                    for (int j = 0; j < width; j++)
                        le[j] = (uint8_t)((uint32_t)v >> (8 * j));
                    av_md5_update(smd5, le, (size_t)width);
                }
            }
            out->format = s.format;
            out->bits = s.bits;
            out->rate = s.rate;
            out->channels = s.channels;
            out->frames += s.samples;
        }
    }
done:
    out->seconds = (double)(clock() - began) / CLOCKS_PER_SEC;
    av_md5_final(md5, digest);
    hex(digest, out->md5);
    av_md5_final(smd5, digest);
    hex(digest, out->source_md5);
    av_free(md5);
    av_free(smd5);
    free(back);
    media_decoder_close(dec);
    media_close(src);
    fclose(f);
    return ok;
}

static const char *pcm_name(int pcm)
{
    return pcm == MEDIA_PCM_S16 ? "s16" : pcm == MEDIA_PCM_S32 ? "s32"
         : pcm == MEDIA_PCM_F32 ? "f32" : "none";
}

int main(int argc, char **argv)
{
    const struct file *f;
    char path[512], want[33], flac[33];
    struct media_stream info;
    struct decoded got, other;
    size_t i;

    if (argc != 2) {
        fprintf(stderr, "usage: test_mediasound <folder for the files it makes>\n");
        return 2;
    }
    dir = argv[1];
    broken = getenv("MEDIASOUNDTEST_BROKEN") != NULL;
    mkdir(dir, 0755);
    printf("test_mediasound: the Media Kit's %s against this Mac's ffmpeg%s\n", media_version(),
           broken ? " - BROKEN: s32 taken through float" : "");

    for (i = 0; i < NFILES; i++) {
        f = &files[i];
        path_of(f, path, sizeof path);
        CHECK(make(f), "%s: the Mac's ffmpeg did not make it", f->name);
    }

    for (i = 0; i < NFILES; i++) {
        f = &files[i];
        path_of(f, path, sizeof path);
        if (!decode(path, MEDIA_PCM_NATIVE, broken && f->pcm == MEDIA_PCM_S32, &info, &got)) {
            CHECK(0, "%s: did not decode", f->name);
            continue;
        }
        CHECK(!strcmp(info.codec, f->codec), "%s: codec %s, not %s", f->name, info.codec, f->codec);
        CHECK(info.rate == f->rate && info.channels == f->channels,
              "%s: %d Hz x %d, not %d x %d", f->name, info.rate, info.channels, f->rate, f->channels);
        CHECK(info.source_bits == f->bits, "%s: source %d bits, not %d", f->name, info.source_bits,
              f->bits);
        CHECK(info.lossless == f->lossless && info.source_float == f->is_float,
              "%s: lossless %d float %d", f->name, info.lossless, info.source_float);
        CHECK(info.pcm == f->pcm, "%s: the stream says %s, not %s", f->name, pcm_name(info.pcm),
              pcm_name(f->pcm));
        CHECK(got.format == (broken && f->pcm == MEDIA_PCM_S32 ? MEDIA_PCM_S32 : f->pcm),
              "%s: handed back %s", f->name, pcm_name(got.format));
        CHECK(got.bits == f->bits, "%s: the sound says %d bits", f->name, got.bits);
        CHECK(got.rate == f->rate && got.channels == f->channels, "%s: sound %d x %d", f->name,
              got.rate, got.channels);
        CHECK(reference(path, f->codec, f->pcm, want), "%s: the Mac's ffmpeg gave no MD5", f->name);
        CHECK(!strcmp(got.md5, want), "%s: %s MD5 %s, the Mac's ffmpeg %s", f->name,
              pcm_name(f->pcm), got.md5, want);
        printf("  %-26s %-9s %6d Hz %2d-bit %s  %ld frames  %s %s\n", f->name, info.codec,
               info.rate, info.source_bits, pcm_name(got.format), got.frames, got.md5,
               strcmp(got.md5, want) ? "DIFFERS" : "= ffmpeg");

        if (!strcmp(f->codec, "flac")) {
            CHECK(streaminfo_md5(path, flac), "%s: no STREAMINFO MD5", f->name);
            CHECK(!strcmp(got.source_md5, flac), "%s: STREAMINFO says %s, the samples %s",
                  f->name, flac, got.source_md5);
            printf("  %-26s STREAMINFO %s %s\n", "", flac,
                   strcmp(got.source_md5, flac) ? "DIFFERS" : "= the samples");
        }

        /* Float holds 24 bits and not 32: why music is not float. */
        if (!broken && f->pcm == MEDIA_PCM_S32
            && decode(path, MEDIA_PCM_NATIVE, 1, &info, &other)) {
            if (f->bits <= 24)
                CHECK(!strcmp(other.md5, got.md5), "%s: %d bits through float changed", f->name,
                      f->bits);
            else
                CHECK(strcmp(other.md5, got.md5), "%s: 32 bits of noise through float the same",
                      f->name);
        }
        /* A 16-bit source widened is ffmpeg's s32 of it. */
        if (f->pcm == MEDIA_PCM_S16 && f->bits == 16
            && decode(path, MEDIA_PCM_S32, 0, &info, &other)) {
            CHECK(other.format == MEDIA_PCM_S32 && reference(path, f->codec, MEDIA_PCM_S32, want)
                  && !strcmp(other.md5, want), "%s: as s32 %s, the Mac's ffmpeg %s", f->name,
                  other.md5, want);
        }
        /* Untrimmed, for the Playback Kit to trim: the encoder's priming
         * left in, so more frames than trimmed, and the same samples as
         * `ffmpeg -flags2 +skip_manual`. */
        if (!f->lossless) {
            long trimmed = got.frames;
            untrim = 1;
            if (decode(path, MEDIA_PCM_NATIVE, 0, &info, &other)) {
                CHECK(other.frames > trimmed, "%s: untrimmed %ld frames, trimmed %ld - the "
                      "priming was not left in", f->name, other.frames, trimmed);
                CHECK(reference(path, f->codec, f->pcm, want) && !strcmp(other.md5, want),
                      "%s: untrimmed %s, the Mac's ffmpeg %s", f->name, other.md5, want);
                printf("  %-26s untrimmed %ld frames (trimmed %ld) %s\n", "", other.frames,
                       trimmed, strcmp(other.md5, want) ? "DIFFERS" : "= ffmpeg");
            } else {
                CHECK(0, "%s: did not decode untrimmed", f->name);
            }
            untrim = 0;
        }
        if (!strcmp(f->codec, "flac") && f->rate == 96000 && got.seconds > 0) {
            printf("  decoding a 96 kHz / 24-bit FLAC of %s: %.2f ms a second of sound "
                   "(%.0fx real time, one core of this Mac)\n",
                   f->signal == NOISE ? "noise" : "a sweep", got.seconds * 1000 / f->seconds,
                   f->seconds / got.seconds);
        }
    }

    printf("test_mediasound: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
