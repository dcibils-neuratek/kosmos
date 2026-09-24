/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A camera's frames into an MP4 of H.264. `record_core.h` says what and why;
 * this says how.
 *
 * **Three steps a frame**: the camera's YUY2 into the encoder's planes -
 * 4:2:0, a U and a V for every two by two pixels (`gfx_yuy2_i420`); those
 * encoded (`H264E_encode`), which gives Annex B - NAL units each after a
 * start code; and each NAL unit handed to the writer with how long its frame
 * is shown, in 90 kHz ticks (`mp4_h26x_write_nal`), which takes the
 * parameter sets for the file's `avcC` and the slices as its samples.
 *
 * **Timed by the camera, not by a count.** A frame is shown until the next
 * one was taken, so a recording made on a machine too slow to encode every
 * frame plays at the speed it happened, with fewer frames in it.
 *
 * **The writer's own memory comes from an arena** at the end of `work`.
 * `minimp4` calls `malloc`, `realloc` and `free`, and a vendored file is not
 * edited: the three names are defined to this file's before it is included
 * (`record_mp4.c`). It asks for two kinds: its sample tables, a few bytes a
 * frame and grown; and two copies of every NAL unit it is handed, taken and
 * given back at once, since it rewrites parameter set IDs as it goes
 * (`MINIMP4_TRANSCODE_SPS_ID`, on in the header as released). So the arena
 * is a stack that takes things back: each block remembers the one below,
 * and freeing the top one pops it and every freed block under it. The
 * copies go as they came; a table grows in place when it is on top.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "record_core.h"
#include "../gfx/yuv.h"

/*
 * The two libraries' declarations; their implementations are compiled in
 * `record_h264.c` and `record_mp4.c`, one each, because both carry the same
 * bitstream writer. `record_config.h` says how, and is the same for all.
 */
#include "record_config.h"
#include "minih264e.h"
#include "minimp4.h"

/* The writer's allocations, from the arena below (`record_mp4.c`). */
void *kosmos_record_malloc(size_t n);
void *kosmos_record_realloc(void *p, size_t n);
void  kosmos_record_free(void *p);

#define ALIGN               64u
#define ARENA_BYTES         (4u * 1024u * 1024u)
#define TICKS               90000u      /* the file's timescale */

struct recorder {
    unsigned  width, height, fps;
    uint64_t  last_us;                  /* the frame before this one */
    unsigned  frames;
    bool      failed;
    bool      full;                     /* a write that did not fit `out` */

    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    H264E_run_param_t run;
    uint8_t  *planes;                   /* Y, then U, then V */
    unsigned  y_stride, uv_stride;

    MP4E_mux_t *mux;
    mp4_h26x_writer_t writer;
    uint8_t  *out;
    size_t    out_bytes;
    size_t    length;                   /* the file's end so far */

    /* The arena: `minimp4`'s memory, a stack of blocks. */
    uint8_t  *arena;
    size_t    arena_used;               /* the end of the top block */
    size_t    arena_top;                /* the top block's header, or NONE */
};

#define ARENA_NONE          ((size_t)-1)

/* Before every block: its size, the block below, and whether it is free. */
struct block {
    size_t size;
    size_t below;
    size_t freed;
    size_t pad;                         /* 32 bytes: payloads 16-aligned */
};

static size_t align_up(size_t n)
{
    return (n + ALIGN - 1u) & ~(size_t)(ALIGN - 1u);
}

/*
 * One recorder at a time is inside `minimp4`, since each call into it is
 * made from here and a process records one camera; this is the one whose
 * arena its allocations come from.
 */
static struct recorder *current;

static struct block *block_at(struct recorder *r, size_t offset)
{
    return (struct block *)(void *)(r->arena + offset);
}

void *kosmos_record_malloc(size_t n)
{
    struct recorder *r = current;
    size_t need = align_up(sizeof(struct block) + n);
    struct block *b;

    if (r == NULL || n > ARENA_BYTES || r->arena_used + need > ARENA_BYTES) {
        return NULL;
    }

    b = block_at(r, r->arena_used);
    b->size = n;
    b->below = r->arena_top;
    b->freed = 0;
    r->arena_top = r->arena_used;
    r->arena_used += need;
    return b + 1;
}

void kosmos_record_free(void *p)
{
    struct recorder *r = current;

    if (p == NULL || r == NULL) {
        return;
    }

    ((struct block *)p - 1)->freed = 1;

    /* The top and every freed block under it, back to the arena. */
    while (r->arena_top != ARENA_NONE && block_at(r, r->arena_top)->freed) {
        r->arena_used = r->arena_top;
        r->arena_top = block_at(r, r->arena_top)->below;
    }
}

void *kosmos_record_realloc(void *p, size_t n)
{
    struct recorder *r = current;
    struct block *b;
    void *to;

    if (p == NULL) {
        return kosmos_record_malloc(n);
    }

    b = (struct block *)p - 1;

    /* The top one grows where it is, which is how a table grows. */
    if (r != NULL && (uint8_t *)b == r->arena + r->arena_top) {
        size_t end = r->arena_top + align_up(sizeof(struct block) + n);

        if (n > ARENA_BYTES || end > ARENA_BYTES) {
            return NULL;
        }

        b->size = n;
        r->arena_used = end;
        return p;
    }

    to = kosmos_record_malloc(n);

    if (to != NULL) {
        memcpy(to, p, b->size < n ? b->size : n);
        kosmos_record_free(p);
    }

    return to;
}

/* The writer's bytes, into `out`; past its end, the recording is full. */
static int rec_write(int64_t offset, const void *buffer, size_t size,
                     void *token)
{
    struct recorder *r = token;

    if (offset < 0 || (uint64_t)offset + size > r->out_bytes) {
        r->failed = true;
        r->full = true;
        return 1;
    }

    memcpy(r->out + offset, buffer, size);

    if ((size_t)offset + size > r->length) {
        r->length = (size_t)offset + size;
    }

    return 0;
}

static void encoder_params(H264E_create_param_t *p, unsigned width,
                           unsigned height, unsigned fps)
{
    memset(p, 0, sizeof(*p));
    p->width = (int)width;
    p->height = (int)height;
    p->gop = (int)(fps * 2u);           /* a key frame every two seconds */
    p->const_input_flag = 1;
    p->enableNEON = 1;
}

size_t recorder_work_bytes(unsigned width, unsigned height)
{
    H264E_create_param_t p;
    int persist = 0, scratch = 0;

    if (width == 0 || height == 0 || (width | height) & 1u
        || width > 4096u || height > 4096u) {
        return 0;
    }

    encoder_params(&p, width, height, 30);

    if (H264E_sizeof(&p, &persist, &scratch) != H264E_STATUS_SUCCESS) {
        return 0;
    }

    return align_up(sizeof(struct recorder)) + align_up((size_t)persist)
         + align_up((size_t)scratch)
         + align_up((size_t)width * height * 3u / 2u) + ARENA_BYTES;
}

struct recorder *recorder_open(void *work, size_t work_bytes, uint8_t *out,
                               size_t out_bytes, unsigned width,
                               unsigned height, unsigned fps,
                               const char **why)
{
    size_t need = recorder_work_bytes(width, height);
    H264E_create_param_t p;
    struct recorder *r;
    int persist = 0, scratch = 0;
    uint8_t *at;

    if (need == 0) {
        *why = "a recording is an even width and height, 4096 at most";
        return NULL;
    }

    if (work == NULL || work_bytes < need || out == NULL || out_bytes < 4096) {
        *why = "not enough memory for a recording";
        return NULL;
    }

    if (fps == 0 || fps > 240) {
        fps = 30;
    }

    memset(work, 0, need);
    r = work;
    at = (uint8_t *)work + align_up(sizeof(*r));

    encoder_params(&p, width, height, fps);
    (void)H264E_sizeof(&p, &persist, &scratch);

    r->enc = (H264E_persist_t *)(void *)at;
    at += align_up((size_t)persist);
    r->scratch = (H264E_scratch_t *)(void *)at;
    at += align_up((size_t)scratch);
    r->planes = at;
    at += align_up((size_t)width * height * 3u / 2u);
    r->arena = at;
    r->arena_top = ARENA_NONE;

    r->width = width;
    r->height = height;
    r->fps = fps;
    r->y_stride = width;
    r->uv_stride = width / 2u;
    r->out = out;
    r->out_bytes = out_bytes;

    if (H264E_init(r->enc, &p) != H264E_STATUS_SUCCESS) {
        *why = "the encoder would not start";
        return NULL;
    }

    /*
     * About a fifth of a bit a pixel, a frame: 1.8 Mbit/s at 640 by 480 and
     * 30 a second - the drawing's 11.2 MB for 42 seconds. The quantiser's
     * range is the encoder's own; the rate is what it aims at.
     */
    r->run.encode_speed = 8;
    r->run.qp_min = 10;
    r->run.qp_max = 51;
    r->run.desired_frame_bytes = (int)((uint64_t)width * height / 40u);

    current = r;
    r->mux = MP4E_open(0, 0, r, rec_write);

    if (r->mux == NULL
        || mp4_h26x_write_init(&r->writer, r->mux, (int)width, (int)height, 0)
           != MP4E_STATUS_OK) {
        current = NULL;
        *why = "the file could not be started";
        return NULL;
    }

    current = NULL;
    return r;
}

/* Where the next NAL unit starts: the next 00 00 01, or the end. */
static size_t next_nal(const uint8_t *at, size_t left)
{
    size_t i;

    for (i = 3; i + 3 <= left; i++) {
        if (at[i] == 0 && at[i + 1] == 0 && at[i + 2] == 1) {
            return (i > 0 && at[i - 1] == 0) ? i - 1 : i;
        }
    }

    return left;
}

bool recorder_yuy2(struct recorder *r, const uint8_t *yuy2, uint64_t time_us,
                   const char **why)
{
    H264E_io_yuv_t io;
    unsigned char *coded = NULL;
    int coded_bytes = 0;
    uint64_t shown_us;
    unsigned ticks;
    size_t at = 0;

    if (r == NULL || r->failed) {
        *why = "the recording has stopped";
        return false;
    }

    gfx_yuy2_i420(r->planes, r->planes + (size_t)r->width * r->height,
                  r->planes + (size_t)r->width * r->height * 5u / 4u,
                  r->y_stride, r->uv_stride, yuy2, r->width, r->height);

    io.yuv[0] = r->planes;
    io.yuv[1] = r->planes + (size_t)r->width * r->height;
    io.yuv[2] = r->planes + (size_t)r->width * r->height * 5u / 4u;
    io.stride[0] = (int)r->y_stride;
    io.stride[1] = (int)r->uv_stride;
    io.stride[2] = (int)r->uv_stride;

    if (H264E_encode(r->enc, r->scratch, &r->run, &io, &coded, &coded_bytes)
        != H264E_STATUS_SUCCESS || coded_bytes <= 0) {
        *why = "the encoder refused a frame";
        return false;
    }

    /* Shown for the time since the one before; the first for a frame's
     * worth at the camera's rate. */
    shown_us = (r->frames == 0 || time_us <= r->last_us)
               ? 1000000u / r->fps : time_us - r->last_us;
    ticks = (unsigned)(shown_us * TICKS / 1000000u);
    r->last_us = time_us;

    if (ticks == 0) {
        ticks = 1;
    }

    current = r;

    while (at < (size_t)coded_bytes) {
        size_t n = next_nal(coded + at, (size_t)coded_bytes - at);

        if (mp4_h26x_write_nal(&r->writer, coded + at, (int)n, ticks)
            != MP4E_STATUS_OK) {
            current = NULL;
            r->failed = true;
            *why = r->full ? "the recording is full"
                           : "the file refused a frame";
            return false;
        }

        at += n;
    }

    current = NULL;
    r->frames++;
    return true;
}

size_t recorder_bytes(const struct recorder *r)
{
    return r != NULL ? r->length : 0;
}

unsigned recorder_frames(const struct recorder *r)
{
    return r != NULL ? r->frames : 0;
}

size_t recorder_close(struct recorder *r, const char **why)
{
    int status;

    if (r == NULL) {
        *why = "no recording";
        return 0;
    }

    current = r;
    status = MP4E_close(r->mux);
    mp4_h26x_write_close(&r->writer);
    current = NULL;

    if (status != MP4E_STATUS_OK || r->frames == 0) {
        *why = r->frames == 0 ? "no frame was recorded"
                              : "the file could not be finished";
        return 0;
    }

    return r->length;
}
