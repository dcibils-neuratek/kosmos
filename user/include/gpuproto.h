/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_GPUPROTO_H
#define KOSMOS_GPUPROTO_H

#include <stdint.h>

/*
 * What you may ask the GPU's driver, at `/Devices/gpu` (`docs/m700-2d.md`).
 * A declared shape, as `backlightproto.h` is: fixed fields each way.
 *
 * **Its first verb is a test, on purpose.** Step 3 of the plan is the cursor
 * plane, and the first write any Kosmos code makes to the M700's display
 * engine is this one: a 64 by 64 picture shown by the cursor plane for some
 * seconds in the middle of the screen, read back, and taken away again,
 * everything as it was. What it says is what the next step - the window
 * manager's pointer handed to the plane - stands on.
 */

#define GPU_OP_LOOK          1u     /* say the display engine again */
#define GPU_OP_CURSOR_TEST   2u     /* `seconds` of a test cursor, then off */

/*
 * **The pointer on the cursor plane** (step 3b): the window manager hands
 * its arrow over once, and then only where it is - so moving the pointer
 * composes nothing, the display drawing it over the frame. `SET` carries
 * the picture's region with the message (64 by 64 words of 0xAARRGGBB at
 * `offset`, `pitch` bytes a row) - control by message, data by shared
 * memory - and `x`, `y` and the hot spot; `MOVE` only `x` and `y`.
 */
#define GPU_OP_CURSOR_SET    3u
#define GPU_OP_CURSOR_MOVE   4u
#define GPU_OP_CURSOR_HIDE   5u

/*
 * **GPU buffers and flips** (step 4b, `m700-2d.md` *Step 4, planned for
 * 3D*): pages the driver owns and enters in the global GTT, handed out as a
 * write-combining region - the screen's two now, a window's, a texture's
 * and a game's swap chain later. Buffer 0 is the firmware's framebuffer.
 *
 * `BUFFER_NEW` takes `x` by `y` pixels, XRGB8888, and answers with the
 * region beside the reply and the buffer's id, GTT address and pitch.
 * `FLIP` (`x`, a buffer) writes the plane's surface and is answered at
 * once: the display takes it at the next vertical blank, and nothing here
 * waits for that. `FLIPPED` says which buffer the display is scanning now
 * and how many flips it has taken - so a buffer is drawn into only once
 * the display has left it.
 */
#define GPU_OP_BUFFER_NEW    6u
#define GPU_OP_FLIP          7u
#define GPU_OP_FLIPPED       8u

/*
 * **What the GPU is doing** (`docs/gpumonitor.md`, step G1): a snapshot of
 * its counters and of the buffers the driver made, answered in a reply of
 * its own shape, `struct gpu_stats`, rather than `gpu_reply`'s seven words.
 * Rates are the caller's: two snapshots' difference over the time between.
 */
#define GPU_OP_STATS         9u

/*
 * **The blitter's first work** (`m700-2d.md` step 5): the GPU woken
 * (forcewake), a ring buffer given to its blit engine in the global GTT,
 * and one command put in it - `MI_STORE_DWORD_IMM`, a number written to a
 * page of memory - and then whether the number arrived. The first time
 * Kosmos submits work to the GPU. Answered in `gpu_reply`'s `engine`.
 */
#define GPU_OP_ENGINE_TEST   10u

/*
 * **The blitter's copies** (`m700-2d.md` step 6). `BUFFER_IMPORT` enters a
 * region the caller hands over - `x` by `y` pixels at `pitch`, from
 * `offset` - in the GTT, page by page, and answers its buffer id; the
 * window server's back buffer is the first, a window's picture next (6b).
 * `BLIT` copies rectangles from one buffer to another, each from its own
 * place in the source, in `struct gpu_blit`'s shape, and answers at once
 * with the fence its last command will write - several may be in flight;
 * `FENCE` says which fence the blitter has reached. `BUFFER_FORGET` gives
 * an imported buffer's slots back.
 */
#define GPU_OP_BUFFER_IMPORT 11u
#define GPU_OP_BLIT          12u
#define GPU_OP_FENCE         13u
#define GPU_OP_BUFFER_FORGET 14u

/*
 * **The first program on the execution units** (`m700-2d.md` 7b): the
 * render engine's GPGPU pipeline runs a kernel that fills a 64 by 4
 * surface with the colour in `x`'s low byte. The engine reply's `value` is
 * how many of its 256 bytes are the colour, `ctl` how many bytes after it
 * were written that should not have been.
 */
#define GPU_OP_COMPUTE_TEST  15u

/*
 * **The render engine's starting state** (7c): i915's batch for a new
 * context, run once - every 3D state given - before the first 3D work.
 * The engine reply's `value` is the fence it reached; asked again, it says
 * so without running it twice.
 */
#define GPU_OP_RENDER_STATE  16u

/*
 * **A texture drawn by the 3D pipeline** (7c): a 64 by 64 picture sampled
 * through the render engine's whole pipeline into another, and held to it
 * pixel for pixel. The engine reply's `value` is how many of the 4096
 * pixels arrived as they are in the picture, `ctl` how many bytes past the
 * destination were written.
 */
#define GPU_OP_RENDER_TEST   17u

#define GPU_BLIT_RECTS       64u
#define GPU_BUFFER_IMPORTED  2u     /* a buffer's kind: a region handed in, shared */

#define GPU_STATS_BUFFERS    16u
#define GPU_DESKTOP_CURSOR   1u     /* the pointer is on the cursor plane */
#define GPU_DESKTOP_FLIPS    2u     /* the screen is shown by flips */
#define GPU_BUFFER_SCANOUT   0u     /* a buffer's kind: a screen */
#define GPU_BUFFER_CURSOR    1u     /*                  the pointer */

#define GPU_OK               0u
#define GPU_ERR_NO_DEVICE    1u     /* no Intel GPU this driver knows */
#define GPU_ERR_BAD_OP       2u
#define GPU_ERR_NO_PIPE      3u     /* no pipe running to show it on */
#define GPU_ERR_NO_MEMORY    4u     /* no contiguous pages for the picture */
#define GPU_ERR_GTT_BUSY     5u     /* the GTT slots chosen were in use */
#define GPU_ERR_UNCHANGED    6u     /* written, and read back different */
#define GPU_ERR_IN_USE       7u     /* a test, while somebody's arrow is shown */
#define GPU_ERR_NO_BUFFER    8u     /* no such buffer, or not the screen's size */
#define GPU_ERR_NO_WAKE      9u     /* forcewake asked for and not acknowledged */
#define GPU_ERR_NO_RING      10u    /* the ring would not take RING_VALID */
#define GPU_ERR_NO_ANSWER    11u    /* the command was given and its number never came */
#define GPU_ERR_BUSY         12u    /* the blitter has too much in flight, or is reading it */

struct gpu_request {
    uint32_t op;
    uint32_t seconds;               /* cursor test: how long it shows, 1 to 30 */
    int32_t  x, y;                  /* the pointer, in the screen's pixels */
    uint32_t hot_x, hot_y;          /* the point of the picture that is there */
    uint32_t offset, pitch;         /* the picture, in the region that came */
};

/*
 * Seven words after the error, meaning what the verb says: for the cursor
 * test the registers as read back - the cursor's control, base and
 * position, its share of the display buffer and its first watermark, and
 * the pipe's underrun flag before and after - and for a buffer or a flip
 * what is named below.
 */
struct gpu_reply {
    uint32_t error;
    union {
        struct {                    /* the cursor test */
            uint32_t curcntr, curbase, curpos, buf_cfg, wm0;
            uint32_t underrun_before, underrun_after;
        } cursor;
        struct {                    /* BUFFER_NEW */
            uint32_t id, gtt, pitch, pages;
        } buffer;
        struct {                    /* ENGINE_TEST */
            uint32_t value;         /* what arrived where the command was to write */
            uint32_t head, tail;    /* the ring's, after: head reaching tail is done */
            uint32_t ctl;           /* RING_CTL: RING_VALID, and RING_WAIT */
            uint32_t mode;          /* RING_MODE_GEN7: execlists' bit should be clear */
            uint32_t esr;           /* RING_ESR: errors the engine saw */
            uint32_t elapsed_us;    /* from the tail written to the number seen */
        } engine;
        struct {                    /* FLIP and FLIPPED */
            uint32_t flips;         /* PIPE_FLIPCOUNT: flips latched */
            uint32_t live;          /* PLANE_SURFLIVE: the GTT address scanned */
            uint32_t shown;         /* the buffer that is, or ~0u for none */
        } flip;
        uint32_t words[7];
    };
};

struct gpu_blit {
    uint32_t op;                    /* GPU_OP_BLIT */
    uint32_t src, dst;              /* buffer ids */
    uint32_t n;                     /* rectangles that follow, up to GPU_BLIT_RECTS */
    struct {
        int32_t x, y, w, h;         /* where, in the destination */
        int32_t sx, sy;             /* and where from, in the source (6b) */
    } rects[GPU_BLIT_RECTS];
};

_Static_assert(sizeof(struct gpu_blit) == 16 + GPU_BLIT_RECTS * 24,
               "a blit is four words and sixty-four rectangles of six");

struct gpu_stats {
    uint32_t error;
    uint32_t device;                /* the PCI device id: 0x1912, an HD 530 */
    uint32_t width, height;         /* the running pipe's */
    uint32_t freq_mhz;              /* now: GEN6_RPSTAT1's, 50/3 MHz a step */
    uint32_t freq_max_mhz;          /* GEN6_RP_STATE_CAP's RP0, 50 MHz a step */
    uint32_t freq_min_mhz;          /*                     RPn */
    uint32_t rpstat_raw;            /* GEN6_RPSTAT1 as read, for step G1's check */
    uint32_t rc6;                   /* GEN6_GT_GFX_RC6: time asleep, 1280 ns a count, wrapping */
    uint32_t flips;                 /* PIPE_FLIPCOUNT: wrapping */
    uint32_t frames;                /* PIPE_FRMCOUNT: wrapping, so the refresh is measured */
    uint32_t underrun;              /* the pipe's underrun bit is set */
    uint32_t desktop;               /* GPU_DESKTOP_* */
    uint32_t shown;                 /* the buffer scanned out, ~0u for none of ours */
    uint32_t nbuffers;
    uint32_t blitter_busy_us;       /* the blit engine's own clock, its copies' time; wrapping */
    struct {
        uint32_t id, holder;        /* the holder's process id; 0 the firmware */
        uint32_t width, height, pages, kind;
    } buffers[GPU_STATS_BUFFERS];
};

_Static_assert(sizeof(struct gpu_stats) == 64 + GPU_STATS_BUFFERS * 24,
               "gpu.lua unpacks sixteen words and then sixteen rows of six");

_Static_assert(sizeof(struct gpu_request) == 32,
               "the GPU request is eight words, gpu.lua packs \"<I4I4i4i4I4I4I4I4\"");
_Static_assert(sizeof(struct gpu_reply) == 32,
               "the GPU reply is eight words, gpu.lua unpacks \"<I4I4I4I4I4I4I4I4\"");

#endif /* KOSMOS_GPUPROTO_H */
