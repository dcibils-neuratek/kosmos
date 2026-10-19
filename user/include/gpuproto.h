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

/*
 * **A window at a window's proportions** (7c): a 280 by 190 part of a 301
 * by 203 picture blended onto an 800 by 600 target at (123, 77), odd sizes
 * and pitches wider than the width. `value` is the target's pixels that
 * pass of 480,000, `mode` the exact ones, `ctl` those outside that changed.
 */
#define GPU_OP_DRAW_TEST     18u

/*
 * **Drawing by the 3D pipeline** (`m700-2d.md` 7d): `struct gpu_blit`'s
 * rectangles, each from `sx`, `sy` in `src` to `x`, `y` in `dst`, drawn by
 * the render engine - copied, or with DRAW_BLEND laid over what is there by
 * their alpha, as the compositor lays a window. Answered at once with the
 * render engine's fence in `engine.value`; GPU_OP_FENCE with `x` 1 says
 * how far the render engine has got.
 */
#define GPU_OP_DRAW          19u
#define GPU_OP_DRAW_BLEND    20u

/*
 * **A program's own kernel** (`compute.md` C2). `KERNEL` hands the driver
 * a kernel's instructions - `x` bytes from `offset` in the region that
 * comes with the message - which it checks instruction by instruction
 * (`intelgpu_kernel.h`, decision D2) and copies into memory of its own, so
 * nothing changes it after the check; answered with its id in
 * `buffer.id`, or GPU_ERR_KERNEL with the instruction refused in
 * `words[1]` and why, a GPU_KERNEL_*, in `words[2]`. `KERNEL_FORGET` lets
 * go of kernel `x`. `RUN` runs one, in `struct gpu_run`'s shape, over a
 * grid of thread groups with the buffers it names bound in its binding
 * table, and answers at once with the render engine's fence in
 * `engine.value` - GPU_OP_FENCE with `x` 1 says when it is reached. A
 * kernel and the buffers a run binds are their holder's alone.
 */
#define GPU_OP_KERNEL        21u
#define GPU_OP_KERNEL_FORGET 22u
#define GPU_OP_RUN           23u

/*
 * **What a run cost the GPU** (`compute.md` C3): `x` a run's fence; the
 * engine's own clock before its first thread and after its last, in
 * nanoseconds, `engine.value` the low word and `engine.head` the high,
 * `engine.tail` the clock's ticks. GPU_ERR_BUSY until the run is done,
 * GPU_ERR_NO_ANSWER for one sixteen runs or more ago.
 */
#define GPU_OP_RUN_TIME      24u

#define GPU_RUN_BUFFERS      8u
#define GPU_RUN_CONSTANTS    256u   /* bytes: r1 onwards, eight registers */

/* How a run's buffer is seen by its kernel: bytes by offset, or a picture. */
#define GPU_VIEW_BYTES       0u     /* SURFTYPE_BUFFER, RAW: untyped and scattered messages */
#define GPU_VIEW_BGRA        1u     /* 2D, B8G8R8A8_UNORM: media block reads and writes */
#define GPU_VIEW_R8          2u     /* 2D, R8_UNORM */
#define GPU_VIEW_R32F        3u     /* 2D, R32_FLOAT */

/* Why a kernel was refused. */
#define GPU_KERNEL_OK        0u
#define GPU_KERNEL_SIZE      1u     /* empty, too large, or not whole instructions */
#define GPU_KERNEL_COMPACT   2u     /* a compacted instruction */
#define GPU_KERNEL_DEBUG     3u     /* a breakpoint */
#define GPU_KERNEL_OPCODE    4u     /* an instruction outside what a kernel may use */
#define GPU_KERNEL_INDIRECT  5u     /* a register addressed through a0 */
#define GPU_KERNEL_REGISTER  6u     /* a register file or architecture register not allowed */
#define GPU_KERNEL_UNIT      7u     /* a send to a unit a kernel may not use */
#define GPU_KERNEL_MESSAGE   8u     /* a message, or an address, the unit may not be sent */
#define GPU_KERNEL_DESC      9u     /* a send whose descriptor is in a register */
#define GPU_KERNEL_BRANCH    10u    /* a branch outside the kernel, or through a register */
#define GPU_KERNEL_END       11u    /* the last instruction does not end the thread */
#define GPU_KERNEL_EOT       12u    /* a thread's end that is predicated, or not to the spawner */

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
#define GPU_ERR_KERNEL       13u    /* a kernel refused: words[1] where, words[2] why */
#define GPU_ERR_NOT_YOURS    14u    /* a kernel or buffer somebody else holds */

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

struct gpu_run {
    uint32_t op;                    /* GPU_OP_RUN */
    uint32_t kernel;                /* from GPU_OP_KERNEL */
    uint32_t simd;                  /* 8 or 16 lanes a thread */
    uint32_t groups_x, groups_y;    /* the grid: a thread a group */
    uint32_t nbuffers;              /* binding-table entries 0 to nbuffers - 1 */
    struct {
        uint32_t id;                /* from GPU_OP_BUFFER_IMPORT */
        uint32_t view;              /* GPU_VIEW_* */
    } buffers[GPU_RUN_BUFFERS];
    uint32_t nconstants;            /* bytes, a multiple of 32: whole registers */
    uint8_t  constants[GPU_RUN_CONSTANTS];
};

_Static_assert(sizeof(struct gpu_run) == 24 + GPU_RUN_BUFFERS * 8 + 4 + GPU_RUN_CONSTANTS,
               "a run is six words, eight pairs, a count and 256 bytes");

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
    uint32_t render_busy_us;        /* the render engine's own clock, programs' runs' time; wrapping */
    struct {
        uint32_t id, holder;        /* the holder's process id; 0 the firmware */
        uint32_t width, height, pages, kind;
    } buffers[GPU_STATS_BUFFERS];
};

_Static_assert(sizeof(struct gpu_stats) == 68 + GPU_STATS_BUFFERS * 24,
               "gpu.lua unpacks seventeen words and then sixteen rows of six");

_Static_assert(sizeof(struct gpu_request) == 32,
               "the GPU request is eight words, gpu.lua packs \"<I4I4i4i4I4I4I4I4\"");
_Static_assert(sizeof(struct gpu_reply) == 32,
               "the GPU reply is eight words, gpu.lua unpacks \"<I4I4I4I4I4I4I4I4\"");

#endif /* KOSMOS_GPUPROTO_H */
