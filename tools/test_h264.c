/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The H.264 Kit's decoder, held on the Mac to FFmpeg's own conformance
 * checksums (`roadmap.md` 4e).
 *
 * `user/kits/ffmpeg/h264_core.c` and the 95 FFmpeg objects under it are
 * compiled here against Kosmos's headers and Kosmos's configuration, as
 * they are for the guest (`test_h264_libc.c` says what the Mac's C library
 * stands in for). Each stream in `tools/h264_conformance.txt` is then
 * decoded through the kit's own entry points - `h264_open`, `h264_send`,
 * `h264_receive`, `h264_finish` - and every picture that comes out is
 * checksummed the way FFmpeg's `framecrc` does it and compared with
 * FFmpeg's reference for that stream, frame by frame, in order.
 *
 * **What that holds**: that the closure `tools/ffmpeg_vendor.py` took is
 * the decoder and not most of it, that the corrections it made to
 * `config.h` changed nothing a picture depends on, and that the kit hands
 * samples over and takes pictures back the way FFmpeg means them to be.
 * H.264 decoding is exact - a conforming decoder produces these bytes and
 * no others - so a single wrong pixel anywhere is a failure.
 *
 * The streams are raw H.264 (Annex B, start codes) and the kit takes what
 * an MP4 holds - the parameter sets in an `avcC` record, and each sample
 * as NAL units behind four-byte lengths - so this splits each stream into
 * access units (H.264 7.4.1.2.3) and repackages it, which is also the
 * shape of a film from a camera. `-f framecrc`'s checksum is Adler-32
 * started from zero over the picture's planes packed without padding.
 *
 * Usage: test_h264 [name ...]      (every stream in the manifest when none)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../user/kits/ffmpeg/h264_core.h"

#define MANIFEST "tools/h264_conformance.txt"
#define STREAMS  "build/downloads/h264-conformance/"
#define REFS     "runtime/upstream/ffmpeg/tests/ref/fate/h264-conformance-"

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

/* FFmpeg's reference: each frame's checksum, and how many bytes it was. */
struct reference {
    uint32_t *crc;
    size_t   *size;
    unsigned  frames;
};

static int read_reference(const char *name, struct reference *r)
{
    char path[256], line[256];
    FILE *f;
    unsigned room = 0;

    snprintf(path, sizeof path, "%s%s", REFS, name);
    f = fopen(path, "r");

    if (f == NULL) {
        return 0;
    }

    memset(r, 0, sizeof *r);

    while (fgets(line, sizeof line, f) != NULL) {
        unsigned long size, crc;
        char *comma;
        int field;

        if (line[0] == '#') {
            continue;
        }

        /* "0, dts, pts, duration, size, 0xcrc" */
        comma = line;
        for (field = 0; field < 4 && comma != NULL; field++) {
            comma = strchr(comma, ',');
            if (comma != NULL) comma++;
        }

        if (comma == NULL || sscanf(comma, " %lu, 0x%lx", &size, &crc) != 2) {
            continue;
        }

        if (r->frames == room) {
            room = room ? room * 2 : 256;
            r->crc = realloc(r->crc, room * sizeof *r->crc);
            r->size = realloc(r->size, room * sizeof *r->size);
        }

        r->crc[r->frames] = (uint32_t)crc;
        r->size[r->frames] = size;
        r->frames++;
    }

    fclose(f);
    return r->frames > 0;
}

/* Adler-32 from zero, as `av_adler32_update(0, ...)` - framecrc's. */
static uint32_t adler(uint32_t sum, const uint8_t *p, size_t n)
{
    uint32_t a = sum & 0xffff, b = sum >> 16;

    while (n-- > 0) {
        a = (a + *p++) % 65521;
        b = (b + a) % 65521;
    }

    return b << 16 | a;
}

static uint32_t picture_crc(const struct h264_picture *p, size_t *bytes)
{
    unsigned cw = (p->width + 1) / 2, ch = (p->height + 1) / 2, row;
    uint32_t sum = 0;

    for (row = 0; row < p->height; row++) {
        sum = adler(sum, p->plane[0] + (long)row * p->stride[0], p->width);
    }
    for (row = 0; row < ch; row++) {
        sum = adler(sum, p->plane[1] + (long)row * p->stride[1], cw);
    }
    for (row = 0; row < ch; row++) {
        sum = adler(sum, p->plane[2] + (long)row * p->stride[2], cw);
    }

    *bytes = (size_t)p->width * p->height + 2 * (size_t)cw * ch;
    return sum;
}

/*
 * One stream's NAL units, found by their start codes: `00 00 01`, with a
 * fourth zero before it counted as the previous unit's trailing zero -
 * which, like any zeros a unit ends with, is not part of it.
 */
struct nal {
    const uint8_t *at;
    size_t         n;
};

static struct nal *split_nals(const uint8_t *s, size_t n, unsigned *count)
{
    struct nal *out = NULL;
    unsigned room = 0, k = 0;
    size_t i = 0, start = 0;
    int in = 0;

    while (i + 3 <= n) {
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
            if (in) {
                size_t end = i;

                while (end > start && s[end - 1] == 0) end--;

                if (k == room) {
                    room = room ? room * 2 : 1024;
                    out = realloc(out, room * sizeof *out);
                }
                out[k].at = s + start;
                out[k].n = end - start;
                k++;
            }
            i += 3;
            start = i;
            in = 1;
        } else {
            i++;
        }
    }

    if (in && start < n) {
        size_t end = n;

        while (end > start && s[end - 1] == 0) end--;

        if (k == room) {
            room = room ? room + 1 : 1;
            out = realloc(out, room * sizeof *out);
        }
        out[k].at = s + start;
        out[k].n = end - start;
        k++;
    }

    *count = k;
    return out;
}

/*
 * The `avcC` record an MP4 would carry for this stream (14496-15
 * 5.3.3.1): its first sequence and picture parameter sets, and four-byte
 * lengths. Later parameter sets stay in the samples, where the decoder
 * reads them as a camera's would be read.
 */
static size_t make_avcc(uint8_t *out, const struct nal *sps,
                        const struct nal *pps)
{
    size_t k = 0;

    out[k++] = 1;
    out[k++] = sps->at[1];
    out[k++] = sps->at[2];
    out[k++] = sps->at[3];
    out[k++] = 0xff;                        /* lengths are four bytes */
    out[k++] = 0xe1;                        /* one SPS */
    out[k++] = (uint8_t)(sps->n >> 8);
    out[k++] = (uint8_t)sps->n;
    memcpy(out + k, sps->at, sps->n);
    k += sps->n;
    out[k++] = 1;                           /* one PPS */
    out[k++] = (uint8_t)(pps->n >> 8);
    out[k++] = (uint8_t)pps->n;
    memcpy(out + k, pps->at, pps->n);
    k += pps->n;
    return k;
}

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

struct run {
    const struct reference *ref;
    unsigned shown;
    unsigned wrong;
    unsigned first_wrong;
    int      errors;
    char     said[160];         /* the first wrong picture, described */
};

static void take(struct h264 *d, struct run *r)
{
    struct h264_picture p;
    enum h264_status s;

    while ((s = h264_receive(d, &p)) == H264_OK) {
        size_t bytes;
        uint32_t crc = picture_crc(&p, &bytes);

        if (r->shown >= r->ref->frames || crc != r->ref->crc[r->shown]
            || bytes != r->ref->size[r->shown]) {
            if (r->wrong == 0) {
                r->first_wrong = r->shown;
                snprintf(r->said, sizeof r->said, "%ux%u, %zu bytes, 0x%08x "
                         "against %zu bytes, 0x%08x", p.width, p.height,
                         bytes, crc,
                         r->shown < r->ref->frames ? r->ref->size[r->shown] : 0,
                         r->shown < r->ref->frames ? r->ref->crc[r->shown] : 0);
            }
            r->wrong++;
        }
        r->shown++;
    }

    if (s == H264_ERROR) {
        r->errors++;
    }
}

static void one_stream(const char *name, const char *file, const char *about)
{
    char path[256], what[480];
    struct reference ref;
    struct nal *nals;
    unsigned count, i, au_start, samples = 0;
    const struct nal *sps = NULL, *pps = NULL;
    uint8_t *data, *avcc, *sample;
    size_t n, sample_room;
    struct h264 *d;
    const char *why = "";
    struct run r;
    double t0;

    snprintf(path, sizeof path, "%s%s", STREAMS, file);
    data = slurp(path, &n);

    if (data == NULL) {
        snprintf(what, sizeof what, "%s: %s is not there - "
                 "tools/fetch_h264_conformance.py fetches it", name, path);
        check(0, what);
        return;
    }

    if (!read_reference(name, &ref)) {
        snprintf(what, sizeof what, "%s: no reference at %s%s", name, REFS,
                 name);
        check(0, what);
        free(data);
        return;
    }

    nals = split_nals(data, n, &count);

    for (i = 0; i < count && (sps == NULL || pps == NULL); i++) {
        unsigned type = nals[i].at[0] & 0x1f;

        if (type == 7 && sps == NULL && nals[i].n >= 4) sps = &nals[i];
        if (type == 8 && pps == NULL) pps = &nals[i];
    }

    if (sps == NULL || pps == NULL) {
        snprintf(what, sizeof what, "%s: no parameter sets found", name);
        check(0, what);
        free(nals);
        free(data);
        return;
    }

    avcc = malloc(16 + sps->n + pps->n);
    d = h264_open(avcc, make_avcc(avcc, sps, pps), &why);
    snprintf(what, sizeof what, "%s: the decoder opens (%s)", name,
             d ? "yes" : why);
    check(d != NULL, what);

    if (d == NULL) {
        free(avcc);
        free(nals);
        free(data);
        return;
    }

    memset(&r, 0, sizeof r);
    r.ref = &ref;
    sample_room = n + 4 * (size_t)count;
    sample = malloc(sample_room);
    t0 = now();

    /*
     * Access units: one begins at a delimiter, a parameter set or SEI after
     * a picture's slices, or at a slice whose first macroblock is 0 when a
     * picture is already under way. `first_mb_in_slice` is the first
     * field of a slice header, ue(v), and 0 is the single bit 1.
     */
    au_start = 0;

    for (i = 0; i <= count; i++) {
        int boundary = (i == count);

        if (!boundary && i > au_start) {
            unsigned type = nals[i].at[0] & 0x1f, j;
            int vcl_before = 0;

            for (j = au_start; j < i; j++) {
                unsigned t = nals[j].at[0] & 0x1f;
                if (t == 1 || t == 5) vcl_before = 1;
            }

            if (vcl_before) {
                if (type == 9 || type == 7 || type == 8 || type == 6
                    || (type >= 14 && type <= 18)) {
                    boundary = 1;
                } else if ((type == 1 || type == 5) && nals[i].n > 1
                           && (nals[i].at[1] & 0x80)) {
                    boundary = 1;
                }
            }
        }

        if (boundary && i > au_start) {
            size_t k = 0;
            unsigned j;
            enum h264_status s;

            for (j = au_start; j < i; j++) {
                size_t len = nals[j].n;

                sample[k++] = (uint8_t)(len >> 24);
                sample[k++] = (uint8_t)(len >> 16);
                sample[k++] = (uint8_t)(len >> 8);
                sample[k++] = (uint8_t)len;
                memcpy(sample + k, nals[j].at, len);
                k += len;
            }

            while ((s = h264_send(d, sample, k, samples)) == H264_AGAIN) {
                take(d, &r);
            }
            if (s == H264_ERROR) r.errors++;
            take(d, &r);
            samples++;
            au_start = i;
        }
    }

    h264_finish(d);
    take(d, &r);

    snprintf(what, sizeof what, "%s (%s): %u pictures of FFmpeg's %u, %u "
             "wrong, the first at %u (%s); %d refused", name, about, r.shown,
             ref.frames, r.wrong, r.first_wrong, r.said, r.errors);
    check(r.shown == ref.frames && r.wrong == 0 && r.errors == 0, what);

    if (r.shown == ref.frames && r.wrong == 0 && r.errors == 0) {
        printf("  ok %-26s %4u pictures, %u samples, %6.1f ms  (%s)\n", name,
               r.shown, samples, (now() - t0) * 1e3, about);
    }

    h264_close(d);
    free(sample);
    free(avcc);
    free(nals);
    free(data);
    free(ref.crc);
    free(ref.size);
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
        char name[64], file[128], digest[80];
        int consumed = 0, i, wanted = argc < 2;

        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }

        if (sscanf(line, "%63s %127s %79s %n", name, file, digest,
                   &consumed) < 3) {
            continue;
        }

        for (i = 1; i < argc; i++) {
            wanted |= strcmp(argv[i], name) == 0;
        }

        if (wanted) {
            line[strcspn(line, "\n")] = '\0';
            one_stream(name, file, line + consumed);
        }
    }

    fclose(m);

    /*
     * And what the kit says to a record that is not `avcC`.
     */
    {
        static const uint8_t not_avcc[8] = { 2, 66, 0, 30, 0xff, 0xe1, 0, 0 };
        const char *why = NULL;
        struct h264 *d = h264_open(not_avcc, sizeof not_avcc, &why);

        check(d == NULL && why != NULL, "a record that is not avcC is "
                                        "refused, with a reason");
    }

    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS",
           checks + fails, fails);
    return fails != 0;
}
