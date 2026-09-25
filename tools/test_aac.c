/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The AAC Kit's decoder, held on the Mac to FFmpeg's own conformance
 * references (`roadmap.md` 4e).
 *
 * `user/kits/ffmpeg/aac_core.c` and the FFmpeg objects under it are
 * compiled here against Kosmos's headers and configuration, as for the
 * guest (`test_h264.c` says how, and `test_h264_libc.c` is shared). Each
 * stream in `tools/aac_conformance.txt` is an MP4, read by the video
 * player's own reader - `tools/mp4index.lua`, run on this Mac's Lua, prints
 * the AudioSpecificConfig and where each frame is - and every frame is
 * decoded through the kit's entry points into sixteen-bit PCM, every
 * channel, and compared with FFmpeg's reference as FFmpeg compares it: the
 * same length, and no sample more than two steps away (`oneoff` with
 * FFmpeg's `FUZZ = 2`).
 *
 * Then the one thing the kit does that FFmpeg's references do not: more
 * than two channels made into two. Each stream with more is decoded again
 * with `stereo` set, and each pair of samples is held to the ITU mix of
 * the reference's channels, weighted by the names the kit reports.
 *
 * Usage: test_aac [name ...]      (every stream in the manifest when none)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../user/kits/ffmpeg/aac_core.h"

#define MANIFEST "tools/aac_conformance.txt"
#define STREAMS  "build/downloads/aac-conformance/"
#define LUA      "build/host/lua"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  not ok: %s\n", what);
    }
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long size;

    if (f == NULL) {
        return NULL;
    }

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

/* The track, as `mp4index.lua` prints it from `/lib/mp4.lua`. */
struct index {
    uint8_t config[64];
    size_t  config_n;
    unsigned rate, channels;            /* as the sample entry states them */
    long   *at;
    long   *size;
    unsigned samples;
};

static int read_index(const char *mp4, struct index *x)
{
    char cmd[512], line[512], hex[160];
    unsigned room = 0;
    FILE *p;
    int ok = 0;

    memset(x, 0, sizeof *x);
    snprintf(cmd, sizeof cmd, "%s tools/mp4index.lua '%s'", LUA, mp4);
    p = popen(cmd, "r");

    if (p == NULL) {
        return 0;
    }

    while (fgets(line, sizeof line, p) != NULL) {
        long at, size;

        if (strncmp(line, "track ", 6) == 0) {
            char *last = strrchr(line, ' ');
            char codec[16];
            unsigned object, aot;
            size_t i;

            sscanf(line, "track %15s %u %u %u %u", codec, &object, &aot,
                   &x->rate, &x->channels);

            if (last == NULL || sscanf(last + 1, "%159s", hex) != 1) {
                break;
            }

            for (i = 0; hex[2 * i] && hex[2 * i + 1]
                        && i < sizeof x->config; i++) {
                unsigned b;

                sscanf(hex + 2 * i, "%2x", &b);
                x->config[i] = (uint8_t)b;
            }
            x->config_n = i;
            ok = 1;
        } else if (sscanf(line, "%ld %ld", &at, &size) == 2) {
            if (x->samples == room) {
                room = room ? room * 2 : 1024;
                x->at = realloc(x->at, room * sizeof *x->at);
                x->size = realloc(x->size, room * sizeof *x->size);
            }
            x->at[x->samples] = at;
            x->size[x->samples] = size;
            x->samples++;
        }
    }

    pclose(p);
    return ok && x->samples > 0;
}

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/*
 * The whole stream through the kit: PCM, its channel count and rate, and
 * how many frames would not decode.
 */
static char names[64][16];              /* the channels, as the kit says */
static char first_refusal[160];

static int16_t *decode_all(const uint8_t *file, size_t file_n,
                           const struct index *x, int stereo, size_t *count,
                           unsigned *channels, unsigned *rate, int *refused,
                           const char **why)
{
    static int16_t frame[AAC_ROOM];
    struct aac *d = aac_open(x->config, x->config_n, x->rate, x->channels,
                             why);
    int16_t *pcm = NULL;
    size_t have = 0, room = 0;
    unsigned i, c;

    *count = 0;
    *refused = 0;
    first_refusal[0] = '\0';

    if (d == NULL) {
        return NULL;
    }

    for (i = 0; i < x->samples; i++) {
        unsigned samples = 0, chans = 0, r = 0;
        size_t n;

        if (x->at[i] < 0 || (size_t)(x->at[i] + x->size[i]) > file_n) {
            (*refused)++;
            continue;
        }

        if (aac_decode(d, file + x->at[i], (size_t)x->size[i], frame,
                       AAC_ROOM, stereo, &samples, &chans, &r) != 0) {
            if (*refused == 0) {
                snprintf(first_refusal, sizeof first_refusal, "frame %u: %s",
                         i, aac_why(d));
            }
            (*refused)++;
            continue;
        }

        for (c = 0; c < 64 && aac_channel(d, c) != NULL; c++) {
            snprintf(names[c], sizeof names[c], "%s", aac_channel(d, c));
        }
        if (c < 64) names[c][0] = '\0';

        n = (size_t)samples * chans;

        if (have + n > room) {
            room = (have + n) * 2;
            pcm = realloc(pcm, room * sizeof *pcm);
        }

        memcpy(pcm + have, frame, n * sizeof *pcm);
        have += n;
        *channels = chans;
        *rate = r;
    }

    aac_close(d);
    *count = have;
    return pcm;
}

static void one_stream(const char *name, const char *file, const char *ref,
                       const char *about)
{
    char path[256], what[512];
    struct index x;
    uint8_t *data, *expect_bytes;
    size_t n, expect_n, count, i;
    unsigned channels = 0, rate = 0;
    int refused, far = 0, worst = 0;
    const char *why = "";
    int16_t *pcm;
    double t0;

    snprintf(path, sizeof path, "%s%s", STREAMS, file);
    data = slurp(path, &n);
    snprintf(path, sizeof path, "%s%s", STREAMS, ref);
    expect_bytes = slurp(path, &expect_n);

    if (data == NULL || expect_bytes == NULL) {
        snprintf(what, sizeof what, "%s: %s or %s is not in %s - "
                 "tools/fetch_conformance.py aac fetches them", name, file,
                 ref, STREAMS);
        check(0, what);
        free(data);
        free(expect_bytes);
        return;
    }

    snprintf(path, sizeof path, "%s%s", STREAMS, file);

    if (!read_index(path, &x)) {
        snprintf(what, sizeof what, "%s: /lib/mp4.lua found no audio track "
                 "in %s", name, file);
        check(0, what);
        free(data);
        free(expect_bytes);
        return;
    }

    t0 = now();
    pcm = decode_all(data, n, &x, 0, &count, &channels, &rate, &refused,
                     &why);

    if (pcm == NULL) {
        snprintf(what, sizeof what, "%s: nothing decoded (%s%s%s)", name,
                 why, first_refusal[0] ? "; " : "", first_refusal);
        check(0, what);
    } else {
        const int16_t *expect = (const int16_t *)(const void *)expect_bytes;
        size_t expect_count = expect_n / 2;
        size_t same = count < expect_count ? count : expect_count;

        for (i = 0; i < same; i++) {
            int d = abs((int)pcm[i] - (int)expect[i]);

            if (d > worst) worst = d;
            if (d > 2) far++;
        }

        snprintf(what, sizeof what, "%s (%s): %zu samples of the "
                 "reference's %zu, %u channels at %u Hz, %d more than two "
                 "steps away (the worst %d), %d frames refused%s%s", name,
                 about, count, expect_count, channels, rate, far, worst,
                 refused, refused ? ", the first " : "", first_refusal);
        check(count == expect_count && far == 0 && refused == 0, what);

        if (count == expect_count && far == 0 && refused == 0) {
            printf("  ok %-20s %8zu samples, %u ch, %6u Hz, worst %d, "
                   "%6.1f ms  (%s)\n", name, count, channels, rate, worst,
                   (now() - t0) * 1e3, about);
        }

        /*
         * More than two channels into two: the kit's own step, held to the
         * ITU mix of the reference, with the weights taken from this
         * test's own table by the names the kit reports for each channel -
         * so a channel mixed by the wrong name, or a name the kit reports
         * wrongly, both show. Mixing samples already rounded is itself a
         * step or so from mixing the floats they came from, so two are
         * allowed.
         */
        if (channels > 2) {
            size_t stereo_count, frames = expect_count / channels;
            unsigned c2 = 0, r2 = 0, c;
            double wl[64], wr[64], sl = 0, sr = 0, scale;
            char layout[256] = "";
            int16_t *two;
            int off = 0, worst2 = 0, clipped = 0;

            for (c = 0; c < channels && c < 64; c++) {
                const char *nm = names[c];
                const double h = 0.70710678;

                wl[c] = wr[c] = 0;
                if (!strcmp(nm, "FL") || !strcmp(nm, "FLC")) wl[c] = 1;
                else if (!strcmp(nm, "FR") || !strcmp(nm, "FRC")) wr[c] = 1;
                else if (!strcmp(nm, "FC")) wl[c] = wr[c] = h;
                else if (!strcmp(nm, "BL") || !strcmp(nm, "SL")) wl[c] = h;
                else if (!strcmp(nm, "BR") || !strcmp(nm, "SR")) wr[c] = h;
                else if (!strcmp(nm, "BC")) wl[c] = wr[c] = 0.5;
                sl += wl[c];
                sr += wr[c];
                strncat(layout, nm, sizeof layout - strlen(layout) - 2);
                strncat(layout, " ", sizeof layout - strlen(layout) - 1);
            }

            scale = (sl > sr ? sl : sr) > 1 ? 1 / (sl > sr ? sl : sr) : 1;
            two = decode_all(data, n, &x, 1, &stereo_count, &c2, &r2,
                             &refused, &why);

            for (i = 0; two != NULL && i < frames
                        && 2 * i + 1 < stereo_count; i++) {
                const int16_t *s = expect + channels * i;
                double l = 0, r = 0;
                int dl, dr, railed = 0;

                /* A reference channel at the rails was clipped before it
                 * was stored, and the kit mixes the float it came from, so
                 * there is nothing here to hold that pair to. Counted. The
                 * references clip symmetrically, at -32767 as well as
                 * 32767 - al07_96's centre channel sits at -32767. */
                for (c = 0; c < channels; c++) {
                    railed |= s[c] >= 32767 || s[c] <= -32767;
                }
                if (railed) {
                    clipped++;
                    continue;
                }

                for (c = 0; c < channels; c++) {
                    l += wl[c] * s[c];
                    r += wr[c] * s[c];
                }
                l *= scale;
                r *= scale;
                dl = abs(two[2 * i] - (int)(l < 0 ? l - 0.5 : l + 0.5));
                dr = abs(two[2 * i + 1] - (int)(r < 0 ? r - 0.5 : r + 0.5));

                if (dl > worst2) worst2 = dl;
                if (dr > worst2) worst2 = dr;
                if (dl > 2 || dr > 2) off++;
            }

            snprintf(what, sizeof what, "%s made stereo from %s: %u "
                     "channels, %zu samples for %zu frames, %d pairs more "
                     "than two steps from the ITU mix (the worst %d; %d "
                     "clipped in the reference)", name, layout, c2,
                     stereo_count, frames, off, worst2, clipped);
            check(two != NULL && c2 == 2 && stereo_count == frames * 2
                  && off == 0, what);

            if (two != NULL && c2 == 2 && off == 0) {
                printf("  ok %-20s made stereo from %s- the ITU mix within "
                       "%d (%d pairs clipped in the reference)\n", name,
                       layout, worst2, clipped);
            }
            free(two);
        }
    }

    free(pcm);
    free(x.at);
    free(x.size);
    free(data);
    free(expect_bytes);
}

int main(int argc, char **argv)
{
    FILE *m = fopen(MANIFEST, "r");
    char line[512];

    if (m == NULL) {
        printf("FAIL: no %s\n", MANIFEST);
        return 1;
    }

    while (fgets(line, sizeof line, m) != NULL) {
        char name[64], file[128], ref[128], d1[80], d2[80];
        int consumed = 0, i, wanted = argc < 2;

        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }

        if (sscanf(line, "%63s %127s %79s %127s %79s %n", name, file, d1,
                   ref, d2, &consumed) < 5) {
            continue;
        }

        for (i = 1; i < argc; i++) {
            wanted |= strcmp(argv[i], name) == 0;
        }

        if (wanted) {
            line[strcspn(line, "\n")] = '\0';
            one_stream(name, file, ref, line + consumed);
        }
    }

    fclose(m);

    {
        static const uint8_t nothing[1] = { 0x12 };
        const char *why = NULL;

        check(aac_open(nothing, sizeof nothing, 0, 0, &why) == NULL
              && why != NULL,
              "a config too short to be one is refused, with a reason");
    }

    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS",
           checks + fails, fails);
    return fails != 0;
}
