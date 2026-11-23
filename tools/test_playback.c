/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Playback Kit's core on the Mac (`docs/playback.md`, step 2): frames
 * into the input ring, `pb_pass` writing periods into an `audio_ring` in
 * memory, and a stand-in for the audio server taking them out.
 *
 * **What it plays must be what the decoder made, unchanged.** Each kind is
 * held to the same sound worked out the plain way: the whole file decoded at
 * once and converted at once by `pcmconvert.h`. At 44.1 kHz, and at a rate
 * that divides it, that is exact - every sample - so a frame dropped,
 * doubled, cut at a record's edge or put in the wrong place shows.
 *
 *   - a WAV, 16-bit stereo at 44.1 kHz, in ragged records through an input
 *     ring small enough to wrap many times: what went in comes out;
 *   - a WAV, 8-bit mono at 11.025 kHz: as converted whole;
 *   - an MP3, ISO's 1 kHz sine (`tools/mp3_conformance.txt`), in pieces of
 *     the file: as minimp3 decodes it whole;
 *   - a seek: a WAV's records of the old place left in the ring, the core
 *     restarted at another, and what comes out is that place and nothing
 *     else. It was held on an AAC until AAC left the kit in 0.12.97
 *     (`docs/media.md` M6d); a WAV's records hold it as well, and exactly;
 *   - nothing to play yet: nothing written, not finished, and not dry -
 *     the input runs dry only once a frame has come;
 *   - a full ring: the pass stops, and goes on when the server has taken;
 *   - decoded songs joined (`docs/player.md` P3): *gapless*, below.
 *
 * Usage: test_playback
 *   PLAYBACKTEST_DIR     where the gapless files are made (build/host/playback)
 *   PLAYBACKTEST_BROKEN  "untrimmed": the control, which must fail
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/kits/playback/playback_core.h"
#include "pcmconvert.h"

#define MP3 "build/downloads/mp3-conformance/sin1k0db.bit"

static int checks, fails;

#define CHECK(ok, ...) do { checks++; if (!(ok)) { fails++; \
    printf("  not ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long size;

    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc((size_t)size + 1);
    if (data == NULL || fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)size;
    return data;
}

/* ------------------------------------------------------------ the stand-ins */

/* A stream's ring, as `audio.open` makes one, in ordinary memory. */
static struct audio_ring *make_ring(uint32_t periods, uint32_t period_bytes)
{
    struct audio_ring *r = calloc(1, AUDIO_RING_DATA + (size_t)periods * period_bytes);

    r->magic = AUDIO_RING_MAGIC;
    r->periods = periods;
    r->period_bytes = period_bytes;
    return r;
}

/* What the audio server would mix, appended to `out`. */
struct heard_pcm { int16_t *s; size_t n, room; };

static void drain(struct audio_ring *r, struct heard_pcm *h)
{
    while (audio_ring_ready(r) > 0) {
        size_t want = r->period_bytes / 2;

        if (h->n + want > h->room) {
            h->room = (h->room + want) * 2;
            h->s = realloc(h->s, h->room * 2);
        }

        memcpy(h->s + h->n, audio_ring_slot(r, r->read), r->period_bytes);
        h->n += want;
        audio_ring_consumed(r, r->read + 1);
    }
}

/*
 * Everything through: put what the input ring takes, pass, drain, until the
 * core says it is finished. `next` hands the records over; false at the end.
 */
typedef int (*source_fn)(void *ctx, const uint8_t **bytes, uint32_t *n,
                         uint32_t *flags);

static int play_all(struct pb *p, struct pb_input *in, struct audio_ring *ring,
                    source_fn next, void *ctx, struct heard_pcm *h)
{
    const uint8_t *bytes = NULL;
    uint32_t n = 0, flags = 0;
    int pending = 0, guard = 0;

    while (!p->heard.finished && guard++ < 10000000) {
        for (;;) {
            if (!pending) {
                if (!next(ctx, &bytes, &n, &flags)) break;
                pending = 1;
            }
            if (!pb_input_put(in, bytes, n, flags)) break;
            pending = 0;
        }

        pb_pass(p, in, ring);
        drain(ring, h);
    }

    return p->heard.finished;
}

/* Two runs of samples, the second padded to whole periods with silence. */
static int same(const int16_t *got, size_t got_n, const int16_t *want,
                size_t want_n, uint32_t period_bytes, const char *what)
{
    size_t padded = (want_n + period_bytes / 2 - 1) / (period_bytes / 2)
                    * (period_bytes / 2);
    size_t first = (size_t)-1;

    for (size_t i = 0; i < got_n && i < want_n; i++) {
        if (got[i] != want[i]) { first = i; break; }
    }

    for (size_t i = want_n; i < got_n && first == (size_t)-1; i++) {
        if (got[i] != 0) first = i;
    }

    CHECK(got_n == padded && first == (size_t)-1,
          "%s: %zu samples heard, %zu wanted (%zu padded), the first wrong at %zd",
          what, got_n, want_n, padded, (ssize_t)first);
    return got_n == padded && first == (size_t)-1;
}

/* The plain way: all of it at once through `pcm_convert`. */
static int16_t *convert_whole(const uint8_t *src, size_t bytes, long rate,
                              long channels, long bits, size_t *out_n)
{
    size_t room = (size_t)((double)bytes / (channels * bits / 8)
                           * 44100.0 / rate + 16) * 2;
    int16_t *out = malloc(room * 2 + 64);
    size_t at = 0, n = 0, used;
    double phase = 0;

    for (;;) {
        long frames = pcm_convert(src + at, bytes - at, rate, channels, bits, 1,
                                  &phase, out + n, 8192, &used);
        n += (size_t)frames * 2;
        at += used;
        if (frames == 0 || at >= bytes) break;
    }

    *out_n = n;
    return out;
}

/* ------------------------------------------------------------ sources */

struct chunks { const uint8_t *data; size_t n, at; unsigned seed; uint32_t most; };

static int chunk_next(void *ctx, const uint8_t **bytes, uint32_t *n, uint32_t *flags)
{
    struct chunks *c = ctx;
    uint32_t len;

    if (c->at >= c->n) return 0;

    c->seed = c->seed * 1103515245u + 12345u;
    len = 1 + (c->seed >> 8) % c->most;
    if (len > c->n - c->at) len = (uint32_t)(c->n - c->at);

    *bytes = c->data + c->at;
    *n = len;
    c->at += len;
    *flags = c->at >= c->n ? PB_END : 0;
    return 1;
}

struct frames { const uint8_t *file; long *at, *size; unsigned count, i; uint32_t gen; };

static int frame_next(void *ctx, const uint8_t **bytes, uint32_t *n, uint32_t *flags)
{
    struct frames *f = ctx;

    if (f->i >= f->count) return 0;

    *bytes = f->file + f->at[f->i];
    *n = (uint32_t)f->size[f->i];
    f->i++;
    *flags = (f->i >= f->count ? PB_END : 0) | (f->gen << 16);
    return 1;
}

/* ------------------------------------------------------------ gapless (player.md P3) */

/*
 * **Decoded songs, joined.** The files are made here, by the Mac's FFmpeg
 * (and `afconvert`, whose AAC carries iTunes's `iTunSMPB`, which FFmpeg's
 * muxer does not write): an original of a 997 Hz tone, a sweep and noise of
 * a fixed seed, cut in two as an album's tracks are, and each piece encoded
 * on its own - no cover attached, since FFmpeg then writes a wrong Xing
 * frame (P2's finding, and a check below for what the kit does with one).
 *
 * Each song is decoded **untrimmed** (`-flags2 +skip_manual`), priming and
 * padding and all, as the Media Kit hands it over, and its delay and length
 * read from the file the way the tag reader does - LAME's header in the
 * Xing/Info frame, `iTunSMPB`'s words - with the decoder's 529 added for an
 * MP3. What the kit makes of that is held to:
 *
 *   - lossless pieces (16-bit WAV at 44.1, 24-bit FLAC at 96): the join is
 *     the uncut original, sample for sample;
 *   - lossy pieces (MP3 as s16, from FFmpeg's fixed-point decoder `mp3`,
 *     which is the one W5 builds and the Media Kit hands over - the float
 *     one's conversion to s16 rounds the first frame after a skip
 *     differently, by one, and is not what the kit is given; AAC as
 *     float): the join is FFmpeg's own gapless decode of the two, sample
 *     for sample; exactly as many frames as the original;
 *     and lined up with it at the cut, at lag nought;
 *   - the second song's mark at the cut's exact frame;
 *   - a seek into the second song at its frame, from a decoder that landed
 *     earlier;
 *   - a second song at another format: the first written, `change` saying
 *     the second's, and the second exact in a ring of its own;
 *   - a length far short of the sound: not believed, the song played out.
 *
 * Control: `PLAYBACKTEST_BROKEN=untrimmed` states every song's delay and
 * length as nought - joining without trimming - and must fail.
 */

#include <stdarg.h>
#include <sys/types.h>

#include "audioproto.h"

static char dir[512];
static int broken_untrimmed;

static int run(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static int run(const char *fmt, ...)
{
    char cmd[4096];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof cmd) return 0;
    return system(cmd) == 0;
}

static uint8_t *fixture(const char *name, size_t *n)
{
    char path[1024];

    snprintf(path, sizeof path, "%s/%s", dir, name);
    return slurp(path, n);
}

#define CUT44  145531u                       /* not a multiple of 1152 or 1024 */
#define CUT96  300007u

static int make_fixtures(void)
{
    const char *ff = "ffmpeg -hide_banner -loglevel error -nostdin -y";

    if (!run("mkdir -p '%s'", dir)) return 0;

    if (!run("command -v ffmpeg >/dev/null && command -v afconvert >/dev/null")) {
        printf("  the gapless checks need the Mac's ffmpeg (Homebrew) and afconvert\n");
        return 0;
    }

    return run("cd '%s' && %s -f lavfi -i 'sine=frequency=997:sample_rate=44100:duration=6'"
               " -f lavfi -i \"aevalsrc=0.2*sin(2*PI*(60*t+900*t*t)):s=44100:d=6\""
               " -f lavfi -i 'anoisesrc=color=white:seed=7:amplitude=0.06:sample_rate=44100:duration=6'"
               " -filter_complex '[0][1][2]amix=inputs=3:normalize=0,aformat=channel_layouts=stereo'"
               " -c:a pcm_s16le orig44.wav", dir, ff)
        && run("cd '%s' && %s -f lavfi -i \"aevalsrc=0.3*sin(2*PI*(40*t+2000*t*t)):s=96000:d=6\""
               " -f lavfi -i 'anoisesrc=color=pink:seed=11:amplitude=0.2:sample_rate=96000:duration=6'"
               " -filter_complex '[0][1]amix=inputs=2:normalize=0,aformat=channel_layouts=stereo'"
               " -c:a pcm_s24le orig96.wav", dir, ff)
        && run("cd '%s' && %s -i orig44.wav -af atrim=end_sample=%u a44.wav"
               " && %s -i orig44.wav -af atrim=start_sample=%u b44.wav"
               " && %s -i orig96.wav -af atrim=end_sample=%u -c:a flac a96.flac"
               " && %s -i orig96.wav -af atrim=start_sample=%u -c:a flac b96.flac",
               dir, ff, CUT44, ff, CUT44, ff, CUT96, ff, CUT96)
        && run("cd '%s' && for x in a b; do"
               " %s -i ${x}44.wav -c:a libmp3lame -b:a 192k ${x}.mp3 &&"
               " afconvert -f m4af -d aac -b 192000 ${x}44.wav ${x}.m4a || exit 1; done",
               dir, ff)
        && run("cd '%s' && %s -i orig44.wav -f s16le orig44.s16 && %s -i orig96.wav -f s32le orig96.s32"
               " && for x in a b; do"
               " %s -i ${x}44.wav -f s16le ${x}44.s16 &&"
               " %s -i ${x}96.flac -f s32le ${x}96.s32 &&"
               " %s -flags2 +skip_manual -c:a mp3 -i ${x}.mp3 -f s16le -ac 2 ${x}.mp3.raw &&"
               " %s -c:a mp3 -i ${x}.mp3 -f s16le -ac 2 ${x}.mp3.trim &&"
               " %s -flags2 +skip_manual -i ${x}.m4a -f f32le -ac 2 ${x}.m4a.raw &&"
               " %s -i ${x}.m4a -f f32le -ac 2 ${x}.m4a.trim || exit 1; done",
               dir, ff, ff, ff, ff, ff, ff, ff, ff);
}

/* LAME's header in an MP3's Xing or Info frame, as `tags.lua` reads it. */
static int lame_gapless(const uint8_t *f, size_t n, uint64_t *delay, uint64_t *length)
{
    for (size_t at = 0; at + 4 < n && at < 65536; at++) {
        uint32_t flags, frames = 0, p, d, pad;
        size_t x;
        int mpeg1, mono, side;

        if (f[at] != 0xff || (f[at + 1] & 0xe6) != 0xe2) continue;   /* layer III sync */

        mpeg1 = ((f[at + 1] >> 3) & 3) == 3;
        mono = (f[at + 3] >> 6) == 3;
        side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        x = at + 4 + (size_t)side;

        if (x + 160 > n || (memcmp(f + x, "Xing", 4) != 0 && memcmp(f + x, "Info", 4) != 0)) {
            return 0;                         /* the first frame says nothing */
        }

        flags = (uint32_t)f[x + 4] << 24 | (uint32_t)f[x + 5] << 16 | (uint32_t)f[x + 6] << 8 | f[x + 7];
        p = 8;
        if (flags & 1) {
            frames = (uint32_t)f[x + p] << 24 | (uint32_t)f[x + p + 1] << 16
                     | (uint32_t)f[x + p + 2] << 8 | f[x + p + 3];
            p += 4;
        }
        if (flags & 2) p += 4;
        if (flags & 4) p += 100;
        if (flags & 8) p += 4;

        d = (uint32_t)f[x + p + 21] << 16 | (uint32_t)f[x + p + 22] << 8 | f[x + p + 23];
        pad = d & 0xfff;
        *delay = (d >> 12) + PB_MP3_DECODER_DELAY;
        *length = (uint64_t)frames * (mpeg1 ? 1152u : 576u) - (d >> 12) - pad;
        return 1;
    }

    return 0;
}

/* iTunes's note: hexadecimal words, the second the priming, the fourth the length. */
static int smpb_gapless(const uint8_t *f, size_t n, uint64_t *delay, uint64_t *length)
{
    for (size_t at = 0; at + 200 < n; at++) {
        unsigned long long w[4];

        if (memcmp(f + at, "iTunSMPB", 8) != 0) continue;

        for (size_t d = at + 8; d + 16 < at + 64; d++) {
            if (memcmp(f + d, "data", 4) == 0) {
                char text[160];

                memcpy(text, f + d + 12, sizeof text - 1);
                text[sizeof text - 1] = '\0';

                if (sscanf(text, " %llx %llx %llx %llx", &w[0], &w[1], &w[2], &w[3]) != 4) return 0;
                *delay = w[1];
                *length = w[3];
                return 1;
            }
        }
    }

    return 0;
}

/* A queue of songs for the feeder: records, then pieces of whole frames. */
struct step { int song; struct pb_song s; const uint8_t *data; size_t bytes; };

struct feed {
    struct step *steps;
    unsigned count, i;
    size_t at;                                /* into the step's data */
    unsigned seed;
    uint32_t gen;
    int ended;
};

static int feed_some(struct feed *f, struct pb_input *in)
{
    int put = 0;

    while (f->i < f->count) {
        struct step *s = &f->steps[f->i];

        if (s->song) {
            if (!pb_input_put(in, &s->s, sizeof s->s, PB_SONG | (f->gen << 16))) return put;
            f->i++;
            f->at = 0;
            put++;
            continue;
        }

        {
            uint32_t fb = s->s.channels * pb_sample_bytes(s->s.format);
            uint32_t len;

            f->seed = f->seed * 1103515245u + 12345u;
            len = (1 + (f->seed >> 8) % 9000u) * fb;
            if (len > s->bytes - f->at) len = (uint32_t)(s->bytes - f->at);

            if (len > 0 && !pb_input_put(in, s->data + f->at, len, f->gen << 16)) return put;
            f->at += len;
            put++;

            if (f->at >= s->bytes) { f->i++; f->at = 0; }
        }
    }

    if (!f->ended && pb_input_put(in, NULL, 0, PB_END | (f->gen << 16))) f->ended = 1;
    return put;
}

struct bytes { uint8_t *b; size_t n, room; };

static void drain_bytes(struct audio_ring *r, struct bytes *h)
{
    while (audio_ring_ready(r) > 0) {
        if (h->n + r->period_bytes > h->room) {
            h->room = (h->room + r->period_bytes) * 2;
            h->b = realloc(h->b, h->room);
        }

        memcpy(h->b + h->n, audio_ring_slot(r, r->read), r->period_bytes);
        h->n += r->period_bytes;
        audio_ring_consumed(r, r->read + 1);
    }
}

/* Until finished, or a change of format. */
static void play_pcm(struct pb *p, struct pb_input *in, struct audio_ring *ring,
                     struct feed *f, struct bytes *h)
{
    for (int guard = 0; guard < 2000000 && !p->heard.finished && !p->heard.change; guard++) {
        feed_some(f, in);
        pb_pass(p, in, ring);
        drain_bytes(ring, h);
    }
}

static struct audio_ring *pcm_ring(uint32_t rate, uint32_t bits, uint32_t channels)
{
    return make_ring(AUDIO_RING_PERIODS,
                     audio_period_frames(256, 44100, rate) * channels * (bits / 8));
}

/* `got` against `want`, the rest of `got` silence to whole periods. */
static void same_bytes(const struct bytes *got, const uint8_t *want, size_t want_n,
                       uint32_t frame, uint32_t period, const char *what)
{
    size_t padded = (want_n + period - 1) / period * period;
    size_t first = (size_t)-1;

    for (size_t i = 0; i < got->n && i < want_n; i++) {
        if (got->b[i] != want[i]) { first = i; break; }
    }

    for (size_t i = want_n; i < got->n && first == (size_t)-1; i++) {
        if (got->b[i] != 0) first = i;
    }

    CHECK(got->n == padded && first == (size_t)-1,
          "%s: %zu frames heard, %zu wanted (%zu padded), the first wrong at frame %zd",
          what, got->n / frame, want_n / frame, padded / frame,
          first == (size_t)-1 ? (ssize_t)-1 : (ssize_t)(first / frame));
}

/* A float sample as the kit must make it: x 2^31, rounded half away, held. */
static int32_t want_f32(float f)
{
    double x = (double)f * 2147483648.0;

    if (x >= 2147483647.0) return 2147483647;
    if (x <= -2147483648.0) return INT32_MIN;
    return (int32_t)(x < 0 ? x - 0.5 : x + 0.5);
}

static uint8_t *f32_to_s32(const uint8_t *f, size_t bytes)
{
    uint8_t *out = malloc(bytes + 1);

    for (size_t i = 0; i + 4 <= bytes; i += 4) {
        float v;
        int32_t s;

        memcpy(&v, f + i, 4);
        s = want_f32(v);
        memcpy(out + i, &s, 4);
    }

    return out;
}

/* 32-bit samples to a 24-bit stream's packed three bytes: the top 24. */
static uint8_t *s32_to_s24(const uint8_t *s, size_t bytes, size_t *out_n)
{
    uint8_t *out = malloc(bytes / 4 * 3 + 1);

    for (size_t i = 0; i < bytes / 4; i++) {
        memcpy(out + 3 * i, s + 4 * i + 1, 3);       /* little-endian: drop the low byte */
    }

    *out_n = bytes / 4 * 3;
    return out;
}

/*
 * Where the join lines up with the original, around the cut: the lag, in
 * -1500..1500 frames, at which the left channel correlates best. Nought
 * for a join without a gap or an overlap.
 */
static int best_lag(const uint8_t *joined, uint32_t joined_bits, size_t joined_frames,
                    const int16_t *orig, size_t orig_frames, size_t cut)
{
    double best = -1e300;
    int lag_best = 99999;

    for (int lag = -1500; lag <= 1500; lag++) {
        double sum = 0;

        for (size_t i = cut - 2048; i < cut + 2048; i++) {
            long j = (long)i + lag;

            if (j < 0 || (size_t)j >= joined_frames || i >= orig_frames) continue;
            int32_t left;

            if (joined_bits == 16) {
                int16_t v;

                memcpy(&v, joined + 4 * (size_t)j, 2);
                left = v;
            } else {
                memcpy(&left, joined + 8 * (size_t)j, 4);
                left >>= 16;
            }

            sum += (double)left * orig[2 * i];
        }

        if (sum > best) { best = sum; lag_best = lag; }
    }

    return lag_best;
}

static struct pb_song song_of(uint32_t id, uint32_t format, uint32_t rate, uint32_t bits,
                              uint64_t delay, uint64_t length)
{
    struct pb_song s = { id, format, rate, 2, bits, 0, delay, length, 0, 0 };

    if (broken_untrimmed) {
        s.delay = 0;
        s.length = 0;
    }

    return s;
}

static void lossless_pair(void)
{
    static struct pb c;
    size_t na, nb, no, n24;
    uint8_t *a = fixture("a44.s16", &na), *b = fixture("b44.s16", &nb),
            *o = fixture("orig44.s16", &no);
    uint8_t *a9 = NULL, *b9 = NULL, *o9 = NULL, *o24;
    size_t na9, nb9, no9;
    struct pb_input *in = malloc(pb_input_bytes(1, 96000, 2, PB_PCM_S32));

    CHECK(a && b && o && na / 4 == CUT44 && na + nb == no,
          "the 44.1 kHz WAV pieces were not made, or are not the original cut in two");
    if (!(a && b && o)) return;

    /* ---- 16-bit WAV pieces at 44.1: the original, sample for sample ---- */
    {
        struct step st[] = {
            { 1, song_of(1, PB_PCM_S16, 44100, 16, 0, na / 4), 0, 0 },
            { 0, song_of(1, PB_PCM_S16, 44100, 16, 0, 0), a, na },
            { 1, song_of(2, PB_PCM_S16, 44100, 16, 0, nb / 4), 0, 0 },
            { 0, song_of(2, PB_PCM_S16, 44100, 16, 0, 0), b, nb },
        };
        struct feed f = { st, 4, 0, 0, 17, 0, 0 };
        struct audio_ring *ring = pcm_ring(44100, 16, 2);
        struct bytes h = { 0 };

        pb_input_init(in, pb_input_bytes(1, 96000, 2, PB_PCM_S32));
        CHECK(pb_open(&c, PB_PCM, 0, 0, 0) == 0 && pb_ring_format(&c, 44100, 16, 2, ring->period_bytes),
              "a decoded player would not open at 44.1/16");
        play_pcm(&c, in, ring, &f, &h);
        CHECK(c.heard.finished && !c.heard.change, "the WAV pair did not finish");
        same_bytes(&h, o, no, 4, ring->period_bytes, "two 16-bit WAV songs joined, against the uncut original");
        CHECK(c.heard.marks == 2 && c.heard.mark[1].song == 2 && c.heard.mark[1].at == CUT44
              && c.heard.mark[1].from == 0 && c.heard.mark[0].at == 0,
              "the second WAV song's mark: song %u at %llu (wanted 2 at %u)", c.heard.mark[1].song,
              (unsigned long long)c.heard.mark[1].at, CUT44);
        free(h.b); free(ring);
    }

    /* ---- 24-bit FLAC pieces at 96 kHz, as s32: a 24-bit stream, exact ---- */
    a9 = fixture("a96.s32", &na9);
    b9 = fixture("b96.s32", &nb9);
    o9 = fixture("orig96.s32", &no9);
    CHECK(a9 && b9 && o9 && na9 / 8 == CUT96 && na9 + nb9 == no9,
          "the 96 kHz FLAC pieces were not made, or do not decode to the original cut in two");

    if (a9 && b9 && o9) {
        struct step st[] = {
            { 1, song_of(1, PB_PCM_S32, 96000, 24, 0, 0), 0, 0 },
            { 0, song_of(1, PB_PCM_S32, 96000, 24, 0, 0), a9, na9 },
            { 1, song_of(2, PB_PCM_S32, 96000, 24, 0, 0), 0, 0 },
            { 0, song_of(2, PB_PCM_S32, 96000, 24, 0, 0), b9, nb9 },
        };
        struct feed f = { st, 4, 0, 0, 23, 0, 0 };
        struct audio_ring *ring = pcm_ring(96000, 24, 2);
        struct bytes h = { 0 };

        CHECK(pb_stream_bits(PB_PCM_S32, 24) == 24 && pb_stream_bits(PB_PCM_S32, 32) == 32
              && pb_stream_bits(PB_PCM_S16, 16) == 16 && pb_stream_bits(PB_PCM_F32, 0) == 32,
              "the bits a song's stream is opened at");
        o24 = s32_to_s24(o9, no9, &n24);
        pb_input_init(in, pb_input_bytes(1, 96000, 2, PB_PCM_S32));
        pb_open(&c, PB_PCM, 0, 0, 0);
        pb_ring_format(&c, 96000, 24, 2, ring->period_bytes);
        play_pcm(&c, in, ring, &f, &h);
        CHECK(c.heard.finished, "the FLAC pair did not finish");
        same_bytes(&h, o24, n24, 6, ring->period_bytes,
                   "two 24-bit FLAC songs at 96 kHz joined, against the uncut original");
        CHECK(c.heard.marks == 2 && c.heard.mark[1].song == 2 && c.heard.mark[1].at == CUT96,
              "the second FLAC song's mark at %llu (wanted %u)",
              (unsigned long long)c.heard.mark[1].at, CUT96);
        free(h.b); free(ring);

        /* ---- a song of another format: 44.1/16, then 96/24 ---- */
        {
            struct step mix[] = {
                { 1, song_of(1, PB_PCM_S16, 44100, 16, 0, 0), 0, 0 },
                { 0, song_of(1, PB_PCM_S16, 44100, 16, 0, 0), a, na },
                { 1, song_of(2, PB_PCM_S32, 96000, 24, 0, 0), 0, 0 },
                { 0, song_of(2, PB_PCM_S32, 96000, 24, 0, 0), b9, nb9 },
            };
            struct feed g = { mix, 4, 0, 0, 29, 0, 0 };
            struct audio_ring *r1 = pcm_ring(44100, 16, 2), *r2 = pcm_ring(96000, 24, 2);
            struct bytes h1 = { 0 }, h2 = { 0 };
            uint32_t ring_before;

            pb_input_init(in, pb_input_bytes(1, 96000, 2, PB_PCM_S32));
            pb_open(&c, PB_PCM, 0, 0, 0);
            pb_ring_format(&c, 44100, 16, 2, r1->period_bytes);
            play_pcm(&c, in, r1, &g, &h1);
            CHECK(c.heard.change && !c.heard.finished && c.heard.change_rate == 96000
                  && c.heard.change_bits == 24 && c.heard.change_channels == 2,
                  "a song at 96/24 after one at 44.1/16: change %d to %u Hz, %u bits",
                  c.heard.change, c.heard.change_rate, c.heard.change_bits);
            same_bytes(&h1, a, na, 4, r1->period_bytes, "the first song, before the change of format");
            ring_before = c.heard.ring;
            CHECK(pb_ring_format(&c, 96000, 24, 2, r2->period_bytes), "the second ring was refused");
            play_pcm(&c, in, r2, &g, &h2);
            CHECK(c.heard.finished, "the second song, at its own format, did not finish");
            same_bytes(&h2, o24 + (size_t)CUT96 * 6, n24 - (size_t)CUT96 * 6, 6, r2->period_bytes,
                       "the second song in a ring of its own format");
            CHECK(c.heard.mark[1].song == 2 && c.heard.mark[1].ring == ring_before + 1
                  && c.heard.mark[1].at == 0,
                  "the second song's mark in the new ring: song %u, ring %u, at %llu",
                  c.heard.mark[1].song, c.heard.mark[1].ring, (unsigned long long)c.heard.mark[1].at);
            free(h1.b); free(h2.b); free(r1); free(r2);
        }

        free(o24);
    }

    free(a); free(b); free(o); free(a9); free(b9); free(o9); free(in);
}

static void lossy_pair(const char *ext, const char *what)
{
    static struct pb c;
    char name[64];
    size_t nfa, nfb, nra, nrb, nta, ntb, no;
    uint8_t *fa, *fb, *ra, *rb, *ta, *tb;
    uint8_t *o = fixture("orig44.s16", &no);
    uint64_t da = 0, la = 0, db = 0, lb = 0;
    int ga, gb;
    /* As the Media Kit decodes them (`media.h`): an MP3 by FFmpeg's
     * fixed-point decoder, as s16; an AAC as float. */
    const uint32_t fmt = strcmp(ext, "mp3") == 0 ? PB_PCM_S16 : PB_PCM_F32;
    const uint32_t ob = fmt == PB_PCM_S16 ? 16 : 32;     /* the stream's bits */
    const uint32_t dfb = fmt == PB_PCM_S16 ? 4 : 8;       /* bytes of a decoded frame */
    const uint32_t sb = fmt == PB_PCM_S16 ? 16 : 0;      /* the source's bits */
    struct pb_input *in = malloc(pb_input_bytes(1, 44100, 2, PB_PCM_F32));

    snprintf(name, sizeof name, "a.%s", ext); fa = fixture(name, &nfa);
    snprintf(name, sizeof name, "b.%s", ext); fb = fixture(name, &nfb);
    snprintf(name, sizeof name, "a.%s.raw", ext); ra = fixture(name, &nra);
    snprintf(name, sizeof name, "b.%s.raw", ext); rb = fixture(name, &nrb);
    snprintf(name, sizeof name, "a.%s.trim", ext); ta = fixture(name, &nta);
    snprintf(name, sizeof name, "b.%s.trim", ext); tb = fixture(name, &ntb);

    if (!(fa && fb && ra && rb && ta && tb && o)) {
        CHECK(0, "%s: the files were not made", what);
        return;
    }

    ga = strcmp(ext, "mp3") == 0 ? lame_gapless(fa, nfa, &da, &la) && lame_gapless(fb, nfb, &db, &lb)
                                 : smpb_gapless(fa, nfa, &da, &la) && smpb_gapless(fb, nfb, &db, &lb);
    gb = la == CUT44 && lb == no / 4 - CUT44;
    CHECK(ga && gb, "%s: the files' gapless notes say %llu and %llu frames (delay %llu, %llu);"
          " the pieces are %u and %zu", what, (unsigned long long)la, (unsigned long long)lb,
          (unsigned long long)da, (unsigned long long)db, CUT44, no / 4 - CUT44);

    /* ---- the pair joined: FFmpeg's own gapless decode, and the original's length ---- */
    {
        struct step st[] = {
            { 1, song_of(1, fmt, 44100, sb, da, la), 0, 0 },
            { 0, song_of(1, fmt, 44100, sb, 0, 0), ra, nra },
            { 1, song_of(2, fmt, 44100, sb, db, lb), 0, 0 },
            { 0, song_of(2, fmt, 44100, sb, 0, 0), rb, nrb },
        };
        struct feed f = { st, 4, 0, 0, 31, 0, 0 };
        struct audio_ring *ring = pcm_ring(44100, ob, 2);
        struct bytes h = { 0 };
        uint8_t *want = malloc(nta + ntb), *wanted;
        char label[128];
        int lag;

        memcpy(want, ta, nta);
        memcpy(want + nta, tb, ntb);
        wanted = fmt == PB_PCM_F32 ? f32_to_s32(want, nta + ntb) : want;

        pb_input_init(in, pb_input_bytes(1, 44100, 2, PB_PCM_F32));
        pb_open(&c, PB_PCM, 0, 0, 0);
        pb_ring_format(&c, 44100, ob, 2, ring->period_bytes);
        play_pcm(&c, in, ring, &f, &h);
        CHECK(c.heard.finished, "%s: the pair did not finish", what);
        snprintf(label, sizeof label, "%s: two songs joined, against FFmpeg's own gapless decode", what);
        same_bytes(&h, wanted, nta + ntb, ob / 4, ring->period_bytes, label);
        CHECK(c.heard.frames_out == no / 4, "%s: %llu frames heard, the original has %zu", what,
              (unsigned long long)c.heard.frames_out, no / 4);
        CHECK(c.heard.marks == 2 && c.heard.mark[1].song == 2 && c.heard.mark[1].at == CUT44,
              "%s: the second song's mark at %llu, the cut at %u", what,
              (unsigned long long)c.heard.mark[1].at, CUT44);
        lag = best_lag(h.b, ob, h.n / (ob / 4), (const int16_t *)o, no / 4, CUT44 + 4000);
        CHECK(lag == 0, "%s: the second song lines up with the original %d frames off", what, lag);
        free(h.b); free(ring); free(want);
        if (wanted != want) free(wanted);
    }

    /* ---- a seek into the second song, the decoder landed earlier ---- */
    {
        const uint64_t to = 40000, landed = 1152 * 30;
        struct pb_song s2 = song_of(2, fmt, 44100, sb, db, lb);
        struct step st[] = {
            { 1, s2, 0, 0 },
            { 0, s2, rb + landed * dfb, nrb - landed * dfb },
        };
        struct feed f = { st, 2, 0, 0, 37, 1, 0 };
        struct audio_ring *ring = pcm_ring(44100, ob, 2);
        struct bytes h = { 0 };
        const uint8_t *tail = tb + to * dfb;
        uint8_t *wanted = fmt == PB_PCM_F32 ? f32_to_s32(tail, ntb - to * dfb) : NULL;
        char label[128];

        st[0].s.first = landed;
        st[0].s.from = to;
        pb_input_init(in, pb_input_bytes(1, 44100, 2, PB_PCM_F32));
        pb_open(&c, PB_PCM, 0, 0, 0);
        /* What a seek leaves behind: the old place's records, of generation 0. */
        pb_input_put(in, &st[0].s, sizeof st[0].s, PB_SONG);
        pb_input_put(in, ra, 65536, 0);
        pb_restart(&c, 1);
        pb_ring_format(&c, 44100, ob, 2, ring->period_bytes);
        play_pcm(&c, in, ring, &f, &h);
        snprintf(label, sizeof label, "%s: a seek to the second song's frame %llu", what,
                 (unsigned long long)to);
        same_bytes(&h, wanted ? wanted : tail, ntb - to * dfb, ob / 4, ring->period_bytes, label);
        CHECK(c.heard.marks == 1 && c.heard.mark[0].song == 2 && c.heard.mark[0].at == 0
              && c.heard.mark[0].from == to,
              "%s: the seek's mark: song %u at %llu from %llu", what, c.heard.mark[0].song,
              (unsigned long long)c.heard.mark[0].at, (unsigned long long)c.heard.mark[0].from);
        free(h.b); free(ring); free(wanted);
    }

    /* ---- a length far short of the sound: not believed ---- */
    if (strcmp(ext, "mp3") == 0 && !broken_untrimmed) {
        /* What FFmpeg's Xing frame says with a cover attached: two frames. */
        struct pb_song s1 = song_of(1, fmt, 44100, sb, da, 2 * 1152 - (da - 529) - 576);
        struct step st[] = { { 1, s1, 0, 0 }, { 0, s1, ra, nra } };
        struct feed f = { st, 2, 0, 0, 41, 0, 0 };
        struct audio_ring *ring = pcm_ring(44100, ob, 2);
        struct bytes h = { 0 };
        const uint8_t *wanted = ra + da * dfb;      /* s16: as decoded */

        pb_input_init(in, pb_input_bytes(1, 44100, 2, PB_PCM_F32));
        pb_open(&c, PB_PCM, 0, 0, 0);
        pb_ring_format(&c, 44100, ob, 2, ring->period_bytes);
        play_pcm(&c, in, ring, &f, &h);
        same_bytes(&h, wanted, nra - da * dfb, ob / 4, ring->period_bytes,
                   "MP3: a stated length of two frames, not believed: the song to its end");
        CHECK(c.heard.untrusted == 1, "MP3: %u songs' lengths not believed, wanted 1",
              c.heard.untrusted);
        free(h.b); free(ring);
    }

    free(fa); free(fb); free(ra); free(rb); free(ta); free(tb); free(o); free(in);
}

static void gapless_checks(void)
{
    const char *d = getenv("PLAYBACKTEST_DIR");
    const char *b = getenv("PLAYBACKTEST_BROKEN");
    int before = fails;

    snprintf(dir, sizeof dir, "%s", d != NULL ? d : "build/host/playback");
    broken_untrimmed = b != NULL && strcmp(b, "untrimmed") == 0;

    if (broken_untrimmed) {
        printf("  control: every song joined untrimmed - this must fail\n");
    }

    if (!make_fixtures()) {
        CHECK(0, "the gapless fixtures could not be made in %s", dir);
        return;
    }

    lossless_pair();
    lossy_pair("mp3", "MP3");
    lossy_pair("m4a", "AAC");

    printf("  gapless: %s\n", fails == before ? "joined sample for sample" : "FAILED");
}

/* ------------------------------------------------------------ the checks */

static struct pb core;

/*
 * The input ring with a guard after it: a record must never run past the
 * ring's end, and one that did would still read back right - the reader
 * follows the writer into the same bytes - so only the guard can see it.
 */
static struct { uint8_t ring[64 + 8192]; uint8_t guard[16384]; } input_area;

static int guard_intact(void)
{
    for (size_t i = 0; i < sizeof(input_area.guard); i++) {
        if (input_area.guard[i] != 0xa5) return 0;
    }
    return 1;
}

int main(void)
{
    struct pb_input *in = (struct pb_input *)input_area.ring;
    const uint32_t period = 1024;

    memset(input_area.guard, 0xa5, sizeof(input_area.guard));

    /* ---- a WAV, 16-bit stereo at 44.1 kHz: what went in comes out ---- */
    {
        size_t n = 44100 * 4 + 1234 * 4;
        int16_t *wav = malloc(n);
        struct chunks c = { (const uint8_t *)wav, n, 0, 3, 3001 };
        struct audio_ring *ring = make_ring(8, period);
        struct heard_pcm h = { 0 };

        for (size_t i = 0; i < n / 2; i++) wav[i] = (int16_t)((i * 7919u) & 0xffff);

        pb_input_init(in, sizeof(input_area.ring));
        CHECK(pb_open(&core, PB_WAV, 44100, 2, 16) == 0, "a 16-bit WAV would not open");
        CHECK(play_all(&core, in, ring, chunk_next, &c, &h), "a 16-bit WAV did not finish");
        same(h.s, h.n, wav, n / 2, period, "16-bit stereo at 44.1 kHz in ragged records");
        CHECK(core.heard.refused == 0, "a WAV refused frames");
        free(h.s); free(ring); free(wav);
    }

    /* ---- a WAV, 8-bit mono at 11.025 kHz: as converted whole ---- */
    {
        size_t n = 11025 + 333, want_n;
        uint8_t *wav = malloc(n);
        struct chunks c = { wav, n, 0, 5, 257 };
        struct audio_ring *ring = make_ring(4, period);
        struct heard_pcm h = { 0 };
        int16_t *want;

        for (size_t i = 0; i < n; i++) wav[i] = (uint8_t)(128 + 100 * ((i / 25) % 2 ? 1 : -1));

        want = convert_whole(wav, n, 11025, 1, 8, &want_n);
        pb_input_init(in, sizeof(input_area.ring));
        CHECK(pb_open(&core, PB_WAV, 11025, 1, 8) == 0, "an 8-bit WAV would not open");
        CHECK(play_all(&core, in, ring, chunk_next, &c, &h), "an 8-bit WAV did not finish");
        same(h.s, h.n, want, want_n, period, "8-bit mono at 11.025 kHz");
        free(h.s); free(ring); free(wav); free(want);
    }

    /* ---- an MP3: as minimp3 decodes the whole file ---- */
    {
        size_t n, want_n, plain_n = 0;
        uint8_t *file = slurp(MP3, &n);

        if (file == NULL) {
            CHECK(0, "%s is not here: tools/fetch_conformance.py mp3", MP3);
        } else {
            static mp3dec_t d;
            int16_t *plain = malloc(n * 40), *want;
            struct chunks c = { file, n, 0, 9, 4096 };
            struct audio_ring *ring = make_ring(8, period);
            struct heard_pcm h = { 0 };
            size_t at = 0;

            mp3dec_init(&d);
            while (at < n) {
                mp3dec_frame_info_t info;
                int s = mp3dec_decode_frame(&d, file + at, (int)(n - at), plain + plain_n, &info);

                if (info.frame_bytes == 0) break;
                at += (size_t)info.frame_bytes;
                plain_n += (size_t)s * (size_t)info.channels;
            }

            want = convert_whole((const uint8_t *)plain, plain_n * 2, 44100, 2, 16, &want_n);
            pb_input_init(in, sizeof(input_area.ring));
            CHECK(pb_open(&core, PB_MP3, 0, 0, 0) == 0, "an MP3 would not open");
            CHECK(play_all(&core, in, ring, chunk_next, &c, &h), "the MP3 did not finish");
            CHECK(core.rate == 44100 && core.channels == 2,
                  "the MP3 was taken as %ld Hz, %ld channels", core.rate, core.channels);
            CHECK(want_n > 44100 * 2 * 8, "the MP3 decoded to only %zu samples", want_n);
            same(h.s, h.n, want, want_n, period, "the 1 kHz MP3 in 4 KB pieces");
            free(h.s); free(ring); free(plain); free(want); free(file);
        }
    }

    /* ---- a seek, on a WAV's records ---- */
    {
        enum { RECORDS = 200, RECORD = 4096 };
        static int16_t pcm[RECORDS * RECORD / 2];
        static long at[RECORDS], size[RECORDS];
        struct frames f = { (const uint8_t *)pcm, at, size, RECORDS, 0, 0 };
        unsigned from = 60;
        struct audio_ring *ring = make_ring(32, period);
        struct heard_pcm before = { 0 }, after = { 0 };
        struct frames old = f, fresh = f;
        const uint8_t *b;
        uint32_t bn, fl;
        size_t want_n;
        int16_t *want;

        for (size_t i = 0; i < sizeof pcm / sizeof pcm[0]; i++) {
            pcm[i] = (int16_t)((i * 7919u) ^ (i >> 3));
        }

        for (unsigned k = 0; k < RECORDS; k++) {
            at[k] = (long)k * RECORD;
            size[k] = RECORD;
        }

        /* A hundred records in, the next twenty put but not played, then
         * restarted at record 60 as a new generation. */
        pb_input_init(in, sizeof(input_area.ring));
        CHECK(pb_open(&core, PB_WAV, 44100, 2, 16) == 0, "the seek's WAV would not open");
        old.count = 100;

        for (int k = 0; k < 4000 && old.i < old.count; k++) {
            if (frame_next(&old, &b, &bn, &fl)) {
                fl &= ~PB_END;
                if (!pb_input_put(in, b, bn, fl)) { old.i--; pb_pass(&core, in, ring); drain(ring, &before); }
            }
        }
        for (unsigned k = 100; k < 120; k++) {
            pb_input_put(in, (const uint8_t *)pcm + at[k], (uint32_t)size[k], 0);
        }

        drain(ring, &before);
        pb_restart(&core, 1);
        fresh.i = from;
        fresh.gen = 1;
        CHECK(play_all(&core, in, ring, frame_next, &fresh, &after), "the seek did not finish");

        want = convert_whole((const uint8_t *)pcm + at[from], (size_t)(RECORDS - from) * RECORD,
                             44100, 2, 16, &want_n);
        same(after.s, after.n, want, want_n, period,
             "a WAV after a seek to record 60, the old place's records left behind");
        free(before.s); free(after.s); free(want); free(ring);
        pb_close(&core);
    }

    /* ---- nothing to play; and a full ring ---- */
    {
        struct audio_ring *ring = make_ring(4, period);
        int16_t tone[3000];                 /* 1500 frames: five periods and some */
        uint32_t w;

        for (int i = 0; i < 3000; i++) tone[i] = (int16_t)(i * 3);

        pb_input_init(in, sizeof(input_area.ring));
        pb_open(&core, PB_WAV, 44100, 2, 16);
        w = pb_pass(&core, in, ring);
        CHECK(w == 0 && core.heard.dry == 0 && !core.heard.finished,
              "nothing to play yet: %u written, %u dry, finished %d", w, core.heard.dry,
              core.heard.finished);

        pb_input_put(in, tone, sizeof(tone), 0);
        w = pb_pass(&core, in, ring);
        CHECK(w == 4 && audio_ring_space(ring) == 0,
              "a full ring: %u written into four, %u space left", w, audio_ring_space(ring));
        ring->read += 2;
        w = pb_pass(&core, in, ring);
        /* 476 frames were left: one whole period, and the rest waits. */
        CHECK(w == 1 && core.heard.dry == 1,
              "after the server took two: %u written, %u dry", w, core.heard.dry);
        free(ring);
    }

    CHECK(guard_intact(), "a record ran past the end of the input ring");

    gapless_checks();

    if (fails) {
        printf("FAIL: %d of %d checks on the Playback Kit's core\n", fails, checks);
        return 1;
    }

    printf("PASS: %d checks on the Playback Kit's core (WAV, MP3, a seek, dry and full;"
           " decoded songs joined gaplessly)\n", checks);
    return 0;
}
