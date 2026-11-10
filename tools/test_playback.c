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
 *   - an AAC, `al05_44` (`tools/aac_conformance.txt`), a frame a record: as
 *     the AAC Kit decodes it frame by frame;
 *   - a seek: records of the old place left in the ring, the core restarted
 *     at another, and what comes out is that place decoded afresh;
 *   - nothing to play: nothing written, a dry pass counted, not finished;
 *   - a full ring: the pass stops, and goes on when the server has taken.
 *
 * Usage: test_playback
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/kits/playback/playback_core.h"
#include "../user/kits/ffmpeg/aac_core.h"
#include "pcmconvert.h"

#define LUA "build/host/lua"
#define MP3 "build/downloads/mp3-conformance/sin1k0db.bit"
#define AAC "build/downloads/aac-conformance/al05_44.mp4"

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

/* The MP4's sound frames, as `tools/mp4index.lua` prints them. */
static int read_index(const char *mp4, uint8_t *config, size_t *config_n,
                      unsigned *rate, unsigned *channels, struct frames *f)
{
    char cmd[512], line[512], hex[160];
    unsigned room = 0;
    FILE *p;
    int ok = 0;

    snprintf(cmd, sizeof cmd, "%s tools/mp4index.lua '%s'", LUA, mp4);
    p = popen(cmd, "r");
    if (p == NULL) return 0;

    while (fgets(line, sizeof line, p) != NULL) {
        long at, size;

        if (strncmp(line, "track ", 6) == 0) {
            char codec[16], *last = strrchr(line, ' ');
            unsigned object, aot;
            size_t i;

            sscanf(line, "track %15s %u %u %u %u", codec, &object, &aot, rate, channels);
            if (last == NULL || sscanf(last + 1, "%159s", hex) != 1) break;
            for (i = 0; hex[2 * i] && hex[2 * i + 1] && i < 64; i++) {
                unsigned b;
                sscanf(hex + 2 * i, "%2x", &b);
                config[i] = (uint8_t)b;
            }
            *config_n = i;
            ok = 1;
        } else if (sscanf(line, "%ld %ld", &at, &size) == 2) {
            if (f->count == room) {
                room = room ? room * 2 : 1024;
                f->at = realloc(f->at, room * sizeof *f->at);
                f->size = realloc(f->size, room * sizeof *f->size);
            }
            f->at[f->count] = at;
            f->size[f->count] = size;
            f->count++;
        }
    }

    pclose(p);
    return ok && f->count > 0;
}

/* The AAC Kit's own way: frames `from`..end decoded with a fresh decoder. */
static uint8_t *aac_plain(const uint8_t *file, const struct frames *f, unsigned from,
                          const uint8_t *config, size_t config_n, unsigned rate,
                          unsigned channels, size_t *bytes)
{
    static int16_t frame[AAC_ROOM];
    const char *why;
    struct aac *d = aac_open(config, config_n, rate, channels, &why);
    uint8_t *pcm = malloc((size_t)f->count * 4096 * 4 + 16);
    size_t n = 0;

    for (unsigned i = from; i < f->count; i++) {
        unsigned s = 0, c = 0, r = 0;

        if (aac_decode(d, file + f->at[i], (size_t)f->size[i], frame, AAC_ROOM,
                       1, &s, &c, &r) == 0) {
            memcpy(pcm + n, frame, (size_t)s * c * 2);
            n += (size_t)s * c * 2;
        }
    }

    aac_close(d);
    *bytes = n;
    return pcm;
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
        CHECK(pb_open(&core, PB_WAV, 44100, 2, 16, NULL, 0) == 0, "a 16-bit WAV would not open");
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
        CHECK(pb_open(&core, PB_WAV, 11025, 1, 8, NULL, 0) == 0, "an 8-bit WAV would not open");
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
            CHECK(pb_open(&core, PB_MP3, 0, 0, 0, NULL, 0) == 0, "an MP3 would not open");
            CHECK(play_all(&core, in, ring, chunk_next, &c, &h), "the MP3 did not finish");
            CHECK(core.rate == 44100 && core.channels == 2,
                  "the MP3 was taken as %ld Hz, %ld channels", core.rate, core.channels);
            CHECK(want_n > 44100 * 2 * 8, "the MP3 decoded to only %zu samples", want_n);
            same(h.s, h.n, want, want_n, period, "the 1 kHz MP3 in 4 KB pieces");
            free(h.s); free(ring); free(plain); free(want); free(file);
        }
    }

    /* ---- an AAC, a frame a record; and a seek ---- */
    {
        size_t n;
        uint8_t *file = slurp(AAC, &n), config[64];
        size_t config_n = 0;
        unsigned rate = 0, channels = 0;
        struct frames f = { 0 };

        if (file == NULL || !read_index(AAC, config, &config_n, &rate, &channels, &f)) {
            CHECK(0, "%s is not here, or %s would not read it: tools/fetch_conformance.py aac",
                  AAC, LUA);
        } else {
            size_t plain_n, want_n;
            uint8_t *plain = aac_plain(file, &f, 0, config, config_n, rate, channels, &plain_n);
            int16_t *want = convert_whole(plain, plain_n, 44100, 2, 16, &want_n);
            struct audio_ring *ring = make_ring(32, period);
            struct heard_pcm h = { 0 };

            f.file = file;
            pb_input_init(in, sizeof(input_area.ring));
            CHECK(pb_open(&core, PB_AAC, rate, channels, 16, config, config_n) == 0,
                  "al05_44 would not open: %s", core.why ? core.why : "?");
            CHECK(play_all(&core, in, ring, frame_next, &f, &h), "al05_44 did not finish");
            CHECK(core.heard.refused == 0, "al05_44: %u frames refused", core.heard.refused);
            same(h.s, h.n, want, want_n, period, "al05_44, a frame a record");
            free(h.s); free(want); free(plain);

            /* A seek: a hundred frames in, the next forty put but not
             * played, then restarted at frame 60 as a new generation. */
            {
                unsigned from = 60;
                struct heard_pcm before = { 0 }, after = { 0 };
                struct frames old = f, fresh = f;
                const uint8_t *b;
                uint32_t bn, fl;

                pb_close(&core);
                pb_input_init(in, sizeof(input_area.ring));
                pb_open(&core, PB_AAC, rate, channels, 16, config, config_n);
                old.i = 0;
                old.count = 100;
                old.gen = 0;

                for (int k = 0; k < 2000 && old.i < old.count; k++) {
                    if (frame_next(&old, &b, &bn, &fl)) {
                        fl &= ~PB_END;
                        if (!pb_input_put(in, b, bn, fl)) { old.i--; pb_pass(&core, in, ring); drain(ring, &before); }
                    }
                }
                for (unsigned k = 100; k < 120; k++) {
                    pb_input_put(in, file + f.at[k], (uint32_t)f.size[k], 0);
                }

                drain(ring, &before);
                pb_restart(&core, 1);
                fresh.i = from;
                fresh.gen = 1;
                CHECK(play_all(&core, in, ring, frame_next, &fresh, &after),
                      "the seek did not finish");

                plain = aac_plain(file, &f, from, config, config_n, rate, channels, &plain_n);
                want = convert_whole(plain, plain_n, 44100, 2, 16, &want_n);
                same(after.s, after.n, want, want_n, period,
                     "al05_44 after a seek to frame 60, the old place's records left behind");
                free(before.s); free(after.s); free(want); free(plain);
            }

            free(ring);
            pb_close(&core);
        }
        free(file);
    }

    /* ---- nothing to play; and a full ring ---- */
    {
        struct audio_ring *ring = make_ring(4, period);
        int16_t tone[3000];                 /* 1500 frames: five periods and some */
        uint32_t w;

        for (int i = 0; i < 3000; i++) tone[i] = (int16_t)(i * 3);

        pb_input_init(in, sizeof(input_area.ring));
        pb_open(&core, PB_WAV, 44100, 2, 16, NULL, 0);
        w = pb_pass(&core, in, ring);
        CHECK(w == 0 && core.heard.dry > 0 && !core.heard.finished,
              "nothing to play: %u written, %u dry, finished %d", w, core.heard.dry,
              core.heard.finished);

        pb_input_put(in, tone, sizeof(tone), 0);
        w = pb_pass(&core, in, ring);
        CHECK(w == 4 && audio_ring_space(ring) == 0,
              "a full ring: %u written into four, %u space left", w, audio_ring_space(ring));
        ring->read += 2;
        w = pb_pass(&core, in, ring);
        /* 476 frames were left: one whole period, and the rest waits. */
        CHECK(w == 1 && core.heard.dry > 1, "after the server took two: %u written", w);
        free(ring);
    }

    CHECK(guard_intact(), "a record ran past the end of the input ring");

    if (fails) {
        printf("FAIL: %d of %d checks on the Playback Kit's core\n", fails, checks);
        return 1;
    }

    printf("PASS: %d checks on the Playback Kit's core (WAV, MP3, AAC, a seek, dry and full)\n",
           checks);
    return 0;
}
