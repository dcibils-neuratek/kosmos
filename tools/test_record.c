/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A camera's frames into an MP4 of H.264, held on the host (`roadmap.md` 6d
 * 8f): `user/kits/record/record_core.c` - the code the Record Kit runs -
 * with `minih264e` and `minimp4` as vendored, recording the camera's own
 * test pattern: eight bars, and a square that moves.
 *
 * **Held to somebody else's decoder.** The file is written to
 * `build/host/test_record.mp4` and read back by FFmpeg - `build/ffmpeg-host`,
 * the decoders built for making the video player's test films - which says
 * what the stream is and how long, and decodes every frame. That FFmpeg has
 * no raw output - its muxers are MOV, MP4 and images, its one encoder MJPEG -
 * so each frame comes back as a JPEG at its best quality, read with the
 * vendored `stb_image`, and its brightness is held to the frame recorded
 * within 30 dB: JPEG costs a few of those, and a frame decoded wrong is far
 * below. Without that FFmpeg the decoding checks are skipped and said to be.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/kits/record/record_core.h"
#include "../user/kits/gfx/yuv.h"

/* Vendored, and compiled on its own terms: its warnings are not ours. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "stb_image.h"
#pragma GCC diagnostic pop

#define FFMPEG "build/ffmpeg-host/ffmpeg"

static int checks, fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  not ok: %s\n", what);
    }
}

/* The driver's pattern (`pattern_draw` in xhci.c), in YUY2. */
static void pattern(uint8_t *to, unsigned w, unsigned h, unsigned phase)
{
    static const uint8_t BARS[8][3] = {
        { 235, 128, 128 }, { 210,  16, 146 }, { 170, 166,  16 },
        { 145,  54,  34 }, { 106, 202, 222 }, {  81,  90, 240 },
        {  41, 240, 110 }, {  16, 128, 128 },
    };
    unsigned x, y, split = h * 3u / 4u, side = h / 8u;
    unsigned at = (phase * 4u) % (w > side ? w - side : 1u);

    for (y = 0; y < h; y++) {
        for (x = 0; x + 1 < w; x += 2) {
            uint8_t *p = to + (size_t)y * w * 2 + x * 2;
            uint8_t yy, u = 128, v = 128;

            if (y < split) {
                const uint8_t *b = BARS[(x * 8u) / w];

                yy = b[0];
                u = b[1];
                v = b[2];
            } else {
                int in = x >= at && x < at + side
                         && y >= split + (h - split - side) / 2u
                         && y < split + (h - split - side) / 2u + side;

                yy = in ? 235 : 16;
            }

            p[0] = yy;
            p[1] = u;
            p[2] = yy;
            p[3] = v;
        }
    }
}

struct take {
    size_t bytes;
    unsigned frames;
    uint8_t *file;
    uint8_t **sources;                  /* each frame recorded, as planes */
};

/*
 * `count` frames `w` by `h`, the i-th taken at `times[i]` microseconds, into
 * an MP4 at `path`. The planes each frame was, kept to compare with.
 */
static int record(const char *path, unsigned w, unsigned h, unsigned count,
                  const uint64_t *times, size_t out_bytes, struct take *t)
{
    size_t work_bytes = recorder_work_bytes(w, h);
    void *work = malloc(work_bytes);
    uint8_t *out = malloc(out_bytes);
    uint8_t *frame = malloc((size_t)w * h * 2);
    const char *why = NULL;
    struct recorder *r;
    unsigned i;
    FILE *f;

    memset(t, 0, sizeof(*t));
    t->sources = calloc(count, sizeof(uint8_t *));
    r = recorder_open(work, work_bytes, out, out_bytes, w, h, 30, &why);

    if (r == NULL) {
        printf("  %ux%u would not open: %s\n", w, h, why);
        return 0;
    }

    for (i = 0; i < count; i++) {
        pattern(frame, w, h, i);
        t->sources[i] = malloc((size_t)w * h * 3 / 2);
        gfx_yuy2_i420_scalar(t->sources[i], t->sources[i] + (size_t)w * h,
                             t->sources[i] + (size_t)w * h * 5 / 4, w, w / 2,
                             frame, w, h);

        if (!recorder_yuy2(r, frame, times[i], &why)) {
            printf("  %ux%u stopped at frame %u: %s\n", w, h, i, why);
            break;
        }
    }

    t->frames = recorder_frames(r);
    t->bytes = recorder_close(r, &why);
    t->file = out;

    if (t->bytes > 0 && path != NULL && (f = fopen(path, "wb")) != NULL) {
        fwrite(out, 1, t->bytes, f);
        fclose(f);
    }

    free(frame);
    free(work);
    return t->bytes > 0;
}

/* FFmpeg's word on a file: its frames as planes, and what it printed. */
static int have_ffmpeg(void)
{
    FILE *f = fopen(FFMPEG, "rb");

    if (f) fclose(f);
    return f != NULL;
}

static double psnr(const uint8_t *a, const uint8_t *b, size_t n)
{
    double sum = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        double d = (double)a[i] - (double)b[i];
        sum += d * d;
    }

    if (sum == 0) return 99.0;
    return 10.0 * log10(255.0 * 255.0 * (double)n / sum);
}

static void decoded(const char *path, unsigned w, unsigned h, unsigned want,
                    struct take *t, const char *what)
{
    char cmd[512], line[512], msg[256], name[256];
    uint8_t *got = malloc((size_t)w * h);
    unsigned frames = 0;
    double worst = 99.0;
    FILE *p;

    system("rm -rf build/host/test_record_frames && "
           "mkdir -p build/host/test_record_frames");
    snprintf(cmd, sizeof cmd, FFMPEG " -v error -i %s -q:v 1 -qmin 1 "
             "-f image2 build/host/test_record_frames/f%%03d.jpg 2>&1", path);
    p = popen(cmd, "r");

    while (p && fgets(line, sizeof line, p)) {
        printf("  FFmpeg: %s", line);          /* a decoding error, said */
    }

    if (p) pclose(p);

    for (;;) {
        int iw = 0, ih = 0, n = 0;
        unsigned char *rgb;
        size_t i;

        snprintf(name, sizeof name, "build/host/test_record_frames/f%03u.jpg",
                 frames + 1);
        rgb = stbi_load(name, &iw, &ih, &n, 3);

        if (rgb == NULL) break;

        if ((unsigned)iw == w && (unsigned)ih == h && frames < t->frames) {
            /* Back to the camera's studio range: 16 + 219/255 of full. */
            for (i = 0; i < (size_t)w * h; i++) {
                double full = 0.299 * rgb[3 * i] + 0.587 * rgb[3 * i + 1]
                            + 0.114 * rgb[3 * i + 2];
                got[i] = (uint8_t)lround(16.0 + full * 219.0 / 255.0);
            }

            {
                double q = psnr(got, t->sources[frames], (size_t)w * h);
                if (q < worst) worst = q;
            }
        } else {
            worst = 0;
        }

        stbi_image_free(rgb);
        frames++;
    }

    snprintf(msg, sizeof msg, "%s: FFmpeg decodes %u frames of %ux%u "
             "(wanted %u)", what, frames, w, h, want);
    check(frames == want, msg);
    snprintf(msg, sizeof msg, "%s: every frame within 30 dB of the one "
             "recorded (the worst %.1f dB)", what, worst);
    check(frames > 0 && worst >= 30.0, msg);

    /* What it says the stream is. */
    snprintf(cmd, sizeof cmd, FFMPEG " -hide_banner -i %s 2>&1", path);
    p = popen(cmd, "r");

    {
        char said[4096] = "";

        while (p && fgets(line, sizeof line, p)) {
            if (strlen(said) + strlen(line) < sizeof said) strcat(said, line);
        }

        if (p) pclose(p);

        snprintf(msg, sizeof msg, "%s: FFmpeg reads it as H.264 in an MP4",
                 what);
        check(strstr(said, "h264") != NULL
              && strstr(said, "mov,mp4") != NULL, msg);

        t->bytes = t->bytes;
        strncpy(line, said, sizeof line - 1);
        line[sizeof line - 1] = '\0';
    }

    free(got);
}

/* FFmpeg's "Duration: 00:00:02.00", in seconds. */
static double duration(const char *path)
{
    char cmd[512], line[512];
    double s = -1;
    FILE *p;

    snprintf(cmd, sizeof cmd, FFMPEG " -hide_banner -i %s 2>&1", path);
    p = popen(cmd, "r");

    while (p && fgets(line, sizeof line, p)) {
        char *at = strstr(line, "Duration: ");
        int hh, mm;
        double ss;

        if (at && sscanf(at, "Duration: %d:%d:%lf", &hh, &mm, &ss) == 3) {
            s = hh * 3600 + mm * 60 + ss;
        }
    }

    if (p) pclose(p);
    return s;
}

int main(void)
{
    enum { N = 60 };
    uint64_t times[N], slow[10];
    struct take t;
    unsigned i;
    char msg[160];
    int ff = have_ffmpeg();

    check(recorder_work_bytes(640, 480) > 0, "640 by 480 can be recorded");
    check(recorder_work_bytes(641, 480) == 0, "an odd width cannot");
    check(recorder_work_bytes(320, 180) > 0,
          "320 by 180 can - 180 is not a multiple of 16, and the encoder "
          "crops");

    /* Two seconds at thirty a second. */
    for (i = 0; i < N; i++) times[i] = 5000000u + (uint64_t)i * 1000000u / 30u;

    check(record("build/host/test_record.mp4", 640, 480, N, times,
                 16u << 20, &t), "sixty frames of 640x480 recorded");
    snprintf(msg, sizeof msg, "%u frames in the file, %zu bytes", t.frames,
             t.bytes);
    check(t.frames == N && t.bytes > 1000, msg);
    check(t.bytes > 8 && memcmp(t.file + 4, "ftyp", 4) == 0,
          "the file starts with its ftyp box");

    if (ff) {
        double d = duration("build/host/test_record.mp4");

        decoded("build/host/test_record.mp4", 640, 480, N, &t, "640x480");
        snprintf(msg, sizeof msg, "two seconds long (FFmpeg: %.2f)", d);
        check(d > 1.9 && d < 2.1, msg);
    }

    /* A size the encoder crops. */
    check(record("build/host/test_record_crop.mp4", 320, 180, 15, times,
                 4u << 20, &t), "320x180 recorded");

    if (ff) {
        decoded("build/host/test_record_crop.mp4", 320, 180, 15, &t,
                "320x180");
    }

    /*
     * Timed by the camera: ten frames a tenth of a second apart - a machine
     * too slow for thirty - make a second of film, not a third of one.
     */
    for (i = 0; i < 10; i++) slow[i] = 1000000u + (uint64_t)i * 100000u;

    check(record("build/host/test_record_slow.mp4", 320, 240, 10, slow,
                 4u << 20, &t), "ten frames a tenth of a second apart");

    if (ff) {
        double d = duration("build/host/test_record_slow.mp4");

        snprintf(msg, sizeof msg, "a second long, as it happened - FFmpeg: "
                 "%.2f s", d);
        check(d > 0.9 && d < 1.1, msg);
    }

    /*
     * A long recording of frames that do not compress - noise - does not run
     * out of the writer's memory: it takes two copies of every NAL unit and
     * gives them back, and an arena that did not take them back filled in a
     * few frames.
     */
    {
        size_t wb = recorder_work_bytes(320, 240);
        void *work = malloc(wb);
        size_t ob = 256u << 20;
        uint8_t *out = malloc(ob), *frame = malloc(320 * 240 * 2);
        const char *why = "";
        struct recorder *r = recorder_open(work, wb, out, ob, 320, 240, 30,
                                           &why);
        unsigned seed = 99u, j, done = 0;

        for (i = 0; r && i < 900; i++) {
            for (j = 0; j < 320u * 240u * 2u; j++) {
                seed = seed * 1103515245u + 12345u;
                frame[j] = (uint8_t)(seed >> 16);
            }

            if (!recorder_yuy2(r, frame, (uint64_t)i * 33333u, &why)) break;
            done++;
        }

        snprintf(msg, sizeof msg, "thirty seconds of noise, 900 frames, "
                 "recorded without the writer running out: %u (%s)", done,
                 done == 900 ? "all" : why);
        check(done == 900, msg);
        check(r && recorder_close(r, &why) > 0, "and closed");
        free(work);
        free(out);
        free(frame);
    }

    /* And a recording that fills its memory stops, and says so. */
    {
        size_t wb = recorder_work_bytes(640, 480);
        void *work = malloc(wb);
        uint8_t *out = malloc(64 * 1024), *frame = malloc(640 * 480 * 2);
        const char *why = "";
        struct recorder *r = recorder_open(work, wb, out, 64 * 1024, 640,
                                           480, 30, &why);
        int stopped = 0;

        unsigned seed = 3u, j;

        for (i = 0; r && i < N && !stopped; i++) {
            for (j = 0; j < 640u * 480u * 2u; j++) {
                seed = seed * 1103515245u + 12345u;
                frame[j] = (uint8_t)(seed >> 16);
            }

            stopped = !recorder_yuy2(r, frame, times[i], &why);
        }

        snprintf(msg, sizeof msg, "a recording that fills 64 KB stops and "
                 "says why: \"%s\"", why);
        check(stopped && strcmp(why, "the recording is full") == 0, msg);
        free(work);
        free(out);
        free(frame);
    }

    if (!ff) {
        printf("SKIP: FFmpeg's decoding, because %s is not built here\n",
               FFMPEG);
    }

    if (fails) {
        printf("FAIL: %d of %d checks on recording\n", fails, checks + fails);
        return 1;
    }

    printf("PASS: %d checks on a camera recorded to H.264 in an MP4%s\n",
           checks, ff ? ", read back by FFmpeg" : "");
    return 0;
}
