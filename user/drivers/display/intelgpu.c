/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The M700's GPU, looked at** (`docs/m700-2d.md` step 2): found, its
 * register window mapped, and the display engine the firmware lit read back
 * - which pipes run and at what size, what each plane scans out, in which
 * format and tiling, from where in the global GTT and so from which physical
 * pages, and whether a cursor plane is on. **Nothing is written**: the
 * display is running, and the first step is a look before any write.
 *
 *   find     the board says where GTTMMADR is              SYS_DEV_FIND
 *   map      its first 2 MB, the registers, uncached        SYS_DEV_MAP
 *            and the GTT's page that a surface's entry is in
 *   read     three pipes, three planes each, a cursor each
 *   say      all of it, a line a thing, and end
 *
 * What it says is the ground the next steps stand on: the cursor plane is
 * the pipe's that runs, page flips write the surface of the plane that is
 * on, and both need the GTT laid out as the firmware left it. Every offset
 * and field is `intelgpu_decode.h`'s, from Linux's i915.
 *
 * No address of its own: GTTMMADR's base is `hal/pc/devices.c`'s.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "gpuproto.h"
#include "intelgpu_decode.h"
#include "intelgpu_fill.h"
#include "intelgpu_kernel.h"
#include "intelgpu_renderstate.h"
#include "intelgpu_render.h"
#include "intelgpu_blend.h"
#include "kits/gfx/surface.h"          /* over(): the compositor's blend, to hold the GPU's to */
#include "mmio.h"
#include "init/say.h"

static uint32_t reg(uintptr_t regs, uint32_t offset)
{
    return mmio_read32(regs + offset);
}

/* One entry of the global GTT, eight bytes, the page holding it mapped. */
static bool gtt_entry(unsigned long gtt_phys, uint32_t surface, uint64_t *entry)
{
    unsigned long index = surface >> 12;
    unsigned long at = gtt_phys + index * 8u;
    long page = kosmos_dev_map(at & ~0xFFFul, 1);

    if (page < 0) {
        return false;
    }

    *entry = (uint64_t)mmio_read32((uintptr_t)page + (at & 0xFFFu))
           | (uint64_t)mmio_read32((uintptr_t)page + (at & 0xFFFu) + 4) << 32;
    return true;
}

static void plane_line(long console, unsigned pipe, unsigned plane,
                       const struct igpu_plane *p, bool mapped, uint64_t entry)
{
    struct say_line line;
    uint64_t physical = 0;
    bool present = mapped && igpu_decode_gtt(entry, &physical);

    say_begin(&line);
    say_text(&line, "intelgpu: pipe ");
    say_text(&line, (const char[]){ (char)('A' + pipe), 0 });
    say_text(&line, " plane ");
    say_dec(&line, plane + 1);
    say_text(&line, ": ");
    say_dec(&line, p->width);
    say_text(&line, "x");
    say_dec(&line, p->height);
    say_text(&line, " at ");
    say_dec(&line, p->x);
    say_text(&line, ",");
    say_dec(&line, p->y);
    say_text(&line, ", ");

    if (p->format != NULL) {
        say_text(&line, p->format);
    } else {
        say_text(&line, "format ");
        say_dec(&line, p->format_code);
    }

    say_text(&line, ", ");
    say_text(&line, p->tiling != NULL ? p->tiling : "unknown tiling");
    say_text(&line, ", ");
    say_dec(&line, p->stride_bytes);
    say_text(&line, " bytes a row, surface at GTT ");
    say_hex(&line, p->surface, 8);

    if (!mapped) {
        say_text(&line, ", its GTT entry not read");
    } else if (!present) {
        say_text(&line, ", its GTT entry not present");
    } else {
        say_text(&line, " = memory ");
        say_hex(&line, (unsigned long)physical, 10);
    }

    say_send(console, &line);
}

/* A plane's share of the display buffer, and its watermarks, a line. */
static void share_line(long console, unsigned pipe, const char *what, uintptr_t regs,
                       uint32_t buf, uint32_t wm0, uint32_t trans)
{
    struct say_line line;
    unsigned start, end, level, lines, blocks;
    bool on;

    igpu_decode_buf(reg(regs, buf), &start, &end);

    say_begin(&line);
    say_text(&line, "intelgpu: pipe ");
    say_text(&line, (const char[]){ (char)('A' + pipe), 0 });
    say_text(&line, " ");
    say_text(&line, what);
    say_text(&line, " buffer ");
    say_dec(&line, start);
    say_text(&line, "-");
    say_dec(&line, end);
    say_text(&line, ", watermarks");

    for (level = 0; level < IGPU_WM_LEVELS; level++) {
        igpu_decode_wm(reg(regs, wm0 + level * 4u), &on, &lines, &blocks);
        say_text(&line, " ");
        say_text(&line, on ? "" : "-");
        say_dec(&line, blocks);
    }

    igpu_decode_wm(reg(regs, trans), &on, &lines, &blocks);
    say_text(&line, ", transition ");
    say_text(&line, on ? "" : "-");
    say_dec(&line, blocks);
    say_send(console, &line);
}

/* What the look found, kept for the requests that follow it. */
static struct {
    bool          found;
    struct dev_info dev;
    uintptr_t     regs;
    int           pipe;             /* the first running, or -1 */
    unsigned      width, height;    /* its source size */
    uint32_t      surface;          /* its plane 1's GTT address */
    unsigned      fb_pages;         /* the pages plane 1 scans out */
    unsigned      fb_pitch;         /* and its bytes a row */
} G = { .pipe = -1 };

static void look(long console)
{
    struct dev_info dev;
    struct say_line line;
    uintptr_t regs;
    long mapped;
    unsigned pipe, plane, running = 0;

    if (kosmos_dev_find(DEV_INTEL_GPU, 0, &dev) != 0) {
        say_begin(&line);
        say_text(&line, "intelgpu: no Intel GPU on this machine - nothing to look at");
        say_send(console, &line);
        return;
    }

    say_begin(&line);
    say_text(&line, "intelgpu: Intel GPU ");
    say_hex(&line, dev.line, 4);
    say_text(&line, " at 00:");
    say_hex(&line, (dev.where >> 3) & 0x1fu, 2);
    say_text(&line, ".");
    say_dec(&line, dev.where & 0x7u);
    say_text(&line, ", GTTMMADR ");
    say_hex(&line, (unsigned long)dev.base, 10);
    say_text(&line, ", ");
    say_dec(&line, (unsigned long)(dev.size >> 20));
    say_text(&line, " MB");
    say_send(console, &line);

    mapped = kosmos_dev_map((unsigned long)dev.base, IGPU_REGISTERS_BYTES / 4096u);

    if (mapped < 0) {
        say_begin(&line);
        say_text(&line, "intelgpu: its registers could not be mapped: ");
        say_dec(&line, (unsigned long)-mapped);
        say_send(console, &line);
        return;
    }

    regs = (uintptr_t)mapped;
    G.found = true;
    G.dev = dev;
    G.regs = regs;
    G.pipe = -1;

    for (pipe = 0; pipe < IGPU_PIPES; pipe++) {
        uint32_t step = pipe * IGPU_PIPE_STRIDE;
        uint32_t conf = reg(regs, IGPU_TRANSCONF_A + step);
        unsigned w, h;
        const char *cursor;

        if (conf == 0xFFFFFFFFu) {
            say_begin(&line);
            say_text(&line, "intelgpu: pipe ");
            say_text(&line, (const char[]){ (char)('A' + pipe), 0 });
            say_text(&line, " does not answer - every bit reads as one");
            say_send(console, &line);
            continue;
        }

        igpu_decode_pipesrc(reg(regs, IGPU_PIPESRC_A + step), &w, &h);

        say_begin(&line);
        say_text(&line, "intelgpu: pipe ");
        say_text(&line, (const char[]){ (char)('A' + pipe), 0 });
        say_text(&line, (conf & (1u << 31)) ? " on, " : " off, ");
        say_dec(&line, w);
        say_text(&line, "x");
        say_dec(&line, h);
        say_text(&line, ", transcoder ");
        say_hex(&line, conf, 8);
        say_send(console, &line);

        if ((conf & (1u << 31)) == 0) {
            continue;
        }

        running++;

        if (G.pipe < 0) {
            G.pipe = (int)pipe;
            G.width = w;
            G.height = h;
        }

        for (plane = 0; plane < IGPU_PLANES; plane++) {
            uint32_t at = step + plane * IGPU_PLANE_STRIDE;
            struct igpu_plane_regs r = {
                reg(regs, IGPU_PLANE_CTL_1_A + at),
                reg(regs, IGPU_PLANE_STRIDE_1_A + at),
                reg(regs, IGPU_PLANE_POS_1_A + at),
                reg(regs, IGPU_PLANE_SIZE_1_A + at),
                reg(regs, IGPU_PLANE_SURF_1_A + at),
                reg(regs, IGPU_PLANE_OFFSET_1_A + at),
            };
            struct igpu_plane p;
            uint64_t entry = 0;
            bool got;

            igpu_decode_plane(&r, &p);

            if (!p.enabled) {
                continue;
            }

            if (G.pipe == (int)pipe && plane == 0) {
                G.surface = p.surface;
                G.fb_pages = (unsigned)(((uint64_t)p.stride_bytes * p.height + 4095u) / 4096u);
            G.fb_pitch = p.stride_bytes;
            }

            got = gtt_entry((unsigned long)dev.base + IGPU_GTT_AT, p.surface, &entry);
            plane_line(console, pipe, plane, &p, got, entry);
        }

        /* The display buffer's shares and watermarks, as the firmware
         * left them: what the cursor plane will need a share of. */
        share_line(console, pipe, "plane 1", regs, step + IGPU_PLANE_BUF_CFG_1_A,
                   step + IGPU_PLANE_WM_1_A_0, step + IGPU_PLANE_WM_TRANS_1_A);
        share_line(console, pipe, "cursor", regs, step + IGPU_CUR_BUF_CFG_A,
                   step + IGPU_CUR_WM_A_0, step + IGPU_CUR_WM_TRANS_A);

        cursor = igpu_cursor_mode(reg(regs, IGPU_CURCNTR_A + step));

        say_begin(&line);
        say_text(&line, "intelgpu: pipe ");
        say_text(&line, (const char[]){ (char)('A' + pipe), 0 });

        if (cursor == NULL) {
            say_text(&line, " cursor plane off");
        } else {
            int x, y;

            igpu_decode_curpos(reg(regs, IGPU_CURPOS_A + step), &x, &y);
            say_text(&line, " cursor plane on, ");
            say_text(&line, cursor);
            say_text(&line, ", base GTT ");
            say_hex(&line, reg(regs, IGPU_CURBASE_A + step), 8);
        }

        say_send(console, &line);
    }

    say_begin(&line);
    say_text(&line, "intelgpu: ");
    say_dec(&line, running);
    say_text(&line, running == 1 ? " pipe running; nothing written" : " pipes running; nothing written");
    say_send(console, &line);
}


/*--------------------------------------------------------------------------
 * **The cursor plane, tried** (`docs/m700-2d.md` step 3): the first writes
 * to the display engine, each read back, and everything put as it was.
 *------------------------------------------------------------------------*/

#define DE_PIPE_IIR_A       0x44408u    /* GEN8_DE_PIPE_IIR, 0x10 a pipe */
#define PIPE_FIFO_UNDERRUN  (1u << 31)
#define GFX_FLSH_CNTL       0x101008u   /* GFX_FLSH_CNTL_GEN6: GTT writes, flushed */
#define CURSOR_ARGB_64      0x27u       /* MCURSOR_MODE_64_ARGB_AX */
#define CURSOR_PAGES        4u          /* 64 x 64 x 4 bytes */
#define CURSOR_BLOCKS       32u         /* skl_cursor_allocation, one pipe on */
#define WM_ENABLE           (1u << 31)

/* The process whose request is being answered: its id, or 0. */
static uint32_t holder_of_request(void)
{
    static struct sender_info who;

    return kosmos_sender(&who) == 0 ? who.id : 0u;
}

static void wreg(uint32_t offset, uint32_t value)
{
    mmio_write32(G.regs + offset, value);
}

/* The picture out of the processor's caches, so the display engine - which
 * does not look in them - reads what was written. */
static void flush_lines(const void *at, size_t bytes)
{
#if defined(__x86_64__)
    for (size_t i = 0; i < bytes; i += 64) {
        __asm__ volatile("clflush (%0)" :: "r"((const char *)at + i) : "memory");
    }

    __asm__ volatile("mfence" ::: "memory");
#else
    (void)at;
    (void)bytes;
#endif
}

/* A test picture: a white frame, orange inside, a clear square in the
 * middle - so alpha is seen to work as well as colour. */
static void test_picture(uint32_t *argb)
{
    for (unsigned y = 0; y < 64; y++) {
        for (unsigned x = 0; x < 64; x++) {
            uint32_t c = 0xFFF26B1Du;

            if (x < 4 || y < 4 || x >= 60 || y >= 60) {
                c = 0xFFFFFFFFu;
            } else if (x >= 24 && x < 40 && y >= 24 && y < 40) {
                c = 0x00000000u;
            }

            argb[y * 64 + x] = c;
        }
    }
}

/*
 * One GTT entry, written: the high word first and the present bit last.
 *
 * Through the whole GTT mapped once, its 8 MB the first time an entry is
 * written: it was a page mapped for each entry, and never unmapped, which
 * four cursor pages could afford and a screen's buffer - nearly five
 * thousand entries - cannot.
 */
#define GTT_ENTRIES (IGPU_GTT_AT / 8u)          /* 8 MB of 8-byte entries */

static uintptr_t gtt_window;

static bool gtt_write(unsigned long index, uint64_t value, uint64_t *was)
{
    uintptr_t p;

    if (index >= GTT_ENTRIES) {
        return false;
    }

    if (gtt_window == 0) {
        long at = kosmos_dev_map((unsigned long)G.dev.base + IGPU_GTT_AT, IGPU_GTT_AT / 4096u);

        if (at < 0) {
            return false;
        }

        gtt_window = (uintptr_t)at;
    }

    p = gtt_window + index * 8u;

    if (was != NULL) {
        *was = (uint64_t)mmio_read32(p) | (uint64_t)mmio_read32(p + 4) << 32;
    }

    mmio_write32(p + 4, (uint32_t)(value >> 32));
    mmio_write32(p, (uint32_t)value);
    return true;
}

/*
 * **The cursor, set up once and kept** (step 3b): its pages, its GTT
 * slots, its share of the display buffer and its watermark. `hide` turns
 * the plane off and keeps the rest, so the next `set` is three register
 * writes; nothing is given back while the driver runs.
 */
static struct {
    bool          ready;
    bool          shown;
    uint32_t      step;
    unsigned long index;            /* its first GTT slot */
    uint32_t     *pixels;           /* 64 x 64, mapped here */
    int           hot_x, hot_y;
    uint32_t      holder;           /* who handed it its picture */
} C;

static uint32_t cursor_prepare(long console)
{
    struct say_line line;
    long region, mapped, phys;
    unsigned i, plane_start, plane_end, buf_start;

    if (C.ready) {
        return GPU_OK;
    }

    if (!G.found) {
        return GPU_ERR_NO_DEVICE;
    }

    if (G.pipe < 0) {
        return GPU_ERR_NO_PIPE;
    }

    C.step = (uint32_t)G.pipe * IGPU_PIPE_STRIDE;
    region = kosmos_mem_create_flags(CURSOR_PAGES, MEM_CONTIGUOUS);

    if (region < 0 || (mapped = kosmos_mem_map(region)) < 0
        || (phys = kosmos_mem_phys(region)) < 0) {
        return GPU_ERR_NO_MEMORY;
    }

    C.pixels = (uint32_t *)mapped;

    /* GTT slots past the framebuffer's, a megabyte on. */
    C.index = (((unsigned long)(G.surface >> 12) + G.fb_pages + 255u) & ~255ul);

    for (i = 0; i < CURSOR_PAGES; i++) {
        if (!gtt_write(C.index + i, (uint64_t)(phys + (long)i * 4096) | 1u, NULL)) {
            return GPU_ERR_GTT_BUSY;
        }
    }

    wreg(GFX_FLSH_CNTL, 1u);
    (void)reg(G.regs, GFX_FLSH_CNTL);

    /* Its share of the display buffer, just past plane 1's, and one
     * watermark - as `cursor_test` proved on the M700 (18.659). */
    igpu_decode_buf(reg(G.regs, IGPU_PLANE_BUF_CFG_1_A + C.step), &plane_start, &plane_end);
    buf_start = plane_end + 1u;
    wreg(IGPU_CUR_BUF_CFG_A + C.step, ((buf_start + CURSOR_BLOCKS - 1u) << 16) | buf_start);
    wreg(IGPU_CUR_WM_A_0 + C.step, WM_ENABLE | (1u << 14) | 8u);

    for (i = 1; i < IGPU_WM_LEVELS; i++) {
        wreg(IGPU_CUR_WM_A_0 + C.step + i * 4u, 0);
    }

    wreg(IGPU_CUR_WM_TRANS_A + C.step, 0);

    C.ready = true;

    say_begin(&line);
    say_text(&line, "intelgpu: the cursor plane ready: picture at memory ");
    say_hex(&line, (unsigned long)phys, 10);
    say_text(&line, ", GTT ");
    say_hex(&line, C.index << 12, 8);
    say_text(&line, ", display buffer ");
    say_dec(&line, buf_start);
    say_text(&line, "-");
    say_dec(&line, buf_start + CURSOR_BLOCKS - 1u);
    say_send(console, &line);

    return GPU_OK;
}

/* `CURPOS`: the picture's top left, each coordinate its own sign bit. */
static uint32_t curpos(int x, int y)
{
    uint32_t v = 0;

    if (x < 0) { v |= 1u << 15; x = -x; }
    if (y < 0) { v |= 1u << 31; y = -y; }

    return v | ((uint32_t)y & 0x7FFFu) << 16 | ((uint32_t)x & 0x7FFFu);
}

/* Position, then base: the base's write is what the rest waits for. */
static void cursor_place(int x, int y)
{
    wreg(IGPU_CURPOS_A + C.step, curpos(x - C.hot_x, y - C.hot_y));
    wreg(IGPU_CURBASE_A + C.step, (uint32_t)(C.index << 12));
}

static void cursor_show(int x, int y)
{
    wreg(IGPU_CURCNTR_A + C.step, CURSOR_ARGB_64);
    cursor_place(x, y);
    C.shown = true;
}

static void cursor_hide(void)
{
    if (C.ready && C.shown) {
        wreg(IGPU_CURCNTR_A + C.step, 0);
        wreg(IGPU_CURBASE_A + C.step, (uint32_t)(C.index << 12));
        C.shown = false;
    }
}

/* The window manager's arrow: copied from its region, row by row. */
static uint32_t cursor_set(long console, long cap, const struct gpu_request *rq)
{
    uint32_t error = cursor_prepare(console);
    long pages, at;
    unsigned row;

    if (error != GPU_OK) {
        return error;
    }

    /* Checked against the region's size before it is mapped: a picture
     * that would run past its end is refused rather than read. */
    if (cap < 0 || rq->hot_x >= 64 || rq->hot_y >= 64 || rq->pitch < 256
        || (pages = kosmos_mem_size(cap)) <= 0
        || (unsigned long)rq->offset + 63ul * rq->pitch + 256ul > (unsigned long)pages * 4096ul
        || (at = kosmos_mem_map(cap)) < 0) {
        return GPU_ERR_BAD_OP;
    }

    for (row = 0; row < 64; row++) {
        __builtin_memcpy(&C.pixels[row * 64],
                         (const char *)at + rq->offset + row * rq->pitch, 256);
    }

    kosmos_share_unmap((unsigned long)at, (unsigned long)pages);

    flush_lines(C.pixels, CURSOR_PAGES * 4096u);
    C.holder = holder_of_request();
    C.hot_x = (int)rq->hot_x;
    C.hot_y = (int)rq->hot_y;
    cursor_show(rq->x, rq->y);
    return GPU_OK;
}

/* The test of 18.659, on the pieces above: a picture of its own in the
 * middle of the screen for `seconds`, read back, and hidden again. */
static void cursor_test(long console, unsigned seconds, struct gpu_reply *rep)
{
    struct say_line line;
    uint32_t error = cursor_prepare(console);

    if (error != GPU_OK || (error = C.shown ? GPU_ERR_IN_USE : GPU_OK) != GPU_OK) {
        rep->error = error;
        return;
    }

    rep->cursor.underrun_before = reg(G.regs, DE_PIPE_IIR_A + 0x10u * (uint32_t)G.pipe) & PIPE_FIFO_UNDERRUN;

    test_picture(C.pixels);
    flush_lines(C.pixels, CURSOR_PAGES * 4096u);
    C.hot_x = C.hot_y = 0;
    cursor_show((int)(G.width - 64u) / 2, (int)(G.height - 64u) / 2);
    kosmos_sleep(25);

    rep->cursor.curcntr = reg(G.regs, IGPU_CURCNTR_A + C.step);
    rep->cursor.curbase = reg(G.regs, IGPU_CURBASE_A + C.step);
    rep->cursor.curpos = reg(G.regs, IGPU_CURPOS_A + C.step);
    rep->cursor.buf_cfg = reg(G.regs, IGPU_CUR_BUF_CFG_A + C.step);
    rep->cursor.wm0 = reg(G.regs, IGPU_CUR_WM_A_0 + C.step);

    if ((rep->cursor.curcntr & 0x27u) != CURSOR_ARGB_64 || rep->cursor.curbase != (uint32_t)(C.index << 12)) {
        rep->error = GPU_ERR_UNCHANGED;
    }

    kosmos_sleep((unsigned long)seconds * 250u);   /* ticks: 250 a second */

    rep->cursor.underrun_after = reg(G.regs, DE_PIPE_IIR_A + 0x10u * (uint32_t)G.pipe) & PIPE_FIFO_UNDERRUN;
    cursor_hide();

    say_begin(&line);
    say_text(&line, "intelgpu: cursor test: read back control ");
    say_hex(&line, rep->cursor.curcntr, 8);
    say_text(&line, ", base ");
    say_hex(&line, rep->cursor.curbase, 8);
    say_text(&line, ", buffer ");
    say_hex(&line, rep->cursor.buf_cfg, 8);
    say_text(&line, "; underrun before ");
    say_text(&line, rep->cursor.underrun_before ? "yes" : "no");
    say_text(&line, ", after ");
    say_text(&line, rep->cursor.underrun_after ? "yes" : "no");
    say_send(console, &line);
}

/*--------------------------------------------------------------------------
 * **GPU buffers and flips** (`docs/m700-2d.md` step 4b): pages the driver
 * owns in the global GTT, handed out as write-combining regions, and the
 * plane's surface pointed at one of them at the next vertical blank.
 *------------------------------------------------------------------------*/

#define BUFFERS_MAX 256u                /* the screens, the back buffer, every window's pictures */

/* The screen shown by flips: from the moment a second screen is made,
 * since a screen buffer is made only to be flipped to - so the desktop's
 * label, asked before the first frame, says so too. */
static bool flips_seen;

static struct buffer {
    bool          used;
    uint32_t      kind;                 /* GPU_BUFFER_SCANOUT, _IMPORTED */
    unsigned long slot;                 /* its first GTT slot */
    uint32_t      holder;               /* the process that asked for it */
    long          cap;                  /* the driver's own hold on it */
    uint32_t      gtt;                  /* its GTT address */
    unsigned long pages;
    unsigned      width, height, pitch;
} B[BUFFERS_MAX];

static unsigned long next_slot;         /* the next free GTT slot past the cursor's */

/* Buffer 0: the firmware's framebuffer, as `look` found it. */
static void buffers_init(void)
{
    if (B[0].used || G.pipe < 0) {
        return;
    }

    B[0].used = true;
    B[0].cap = -1;
    B[0].gtt = G.surface;
    B[0].pages = G.fb_pages;
    B[0].width = G.width;
    B[0].height = G.height;
    B[0].pitch = G.fb_pitch;

    /* Past the framebuffer, then past the cursor's slots, a megabyte
     * aligned: linear surfaces want 256 KB on Gen9 (i915's
     * `intel_linear_alignment`), and a megabyte is a whole number of them. */
    next_slot = (((unsigned long)(G.surface >> 12) + G.fb_pages + 255u) & ~255ul);
    next_slot = (next_slot + CURSOR_PAGES + 255u) & ~255ul;
}

/*--------------------------------------------------------------------------
 * **The blitter's first work** (`docs/m700-2d.md` step 5): forcewake, a
 * ring, one command, and the number it writes looked for. The sequence is
 * i915's legacy `xcs_resume` (`intel_ring_submission.c`, Linux 6.12): stop
 * the ring, point the status page, start, head and tail at nought, the
 * size and RING_VALID, wait for it to take, take STOP_RING away - and then
 * the tail moved past what was written is the doorbell.
 *------------------------------------------------------------------------*/

/* Pages of our own, write-combining and in one run, entered in GTT slots
 * past the ones already given out: their address for the processor and for
 * the GPU. */
static bool gtt_pages(unsigned long pages, volatile uint32_t **cpu, uint32_t *gtt)
{
    long region = kosmos_mem_create_flags(pages, MEM_CONTIGUOUS | MEM_WRITE_COMBINE);
    long at, phys;
    unsigned long i;

    buffers_init();

    if (region < 0 || (at = kosmos_mem_map(region)) < 0 || (phys = kosmos_mem_phys(region)) < 0
        || next_slot + pages > GTT_ENTRIES) {
        return false;
    }

    for (i = 0; i < pages; i++) {
        if (!gtt_write(next_slot + i, (uint64_t)(phys + (long)(i * 4096u)) | 1u, NULL)) {
            return false;
        }
    }

    wreg(GFX_FLSH_CNTL, 1u);
    (void)reg(G.regs, GFX_FLSH_CNTL);

    *cpu = (volatile uint32_t *)at;
    *gtt = (uint32_t)(next_slot << 12);
    next_slot = (next_slot + pages + 255u) & ~255ul;
    return true;
}

/*
 * The GT kept awake: asked, then its acknowledgement looked for - at once
 * for a few thousand reads, then a scheduler tick at a time, fifty at most
 * (i915 allows 50 ms). Released the same way. Held from a submission until
 * its fence is seen, so the engine's registers answer while it works, and
 * let go after, so the GT can sleep in RC6 again.
 */
static bool awake;

static bool forcewake(bool on)
{
    unsigned i;

    if (awake == on) {
        return true;
    }

    wreg(IGPU_FORCEWAKE_GT, (IGPU_FORCEWAKE_KERNEL << 16) | (on ? IGPU_FORCEWAKE_KERNEL : 0u));

    for (i = 0; i < 5000 + 50; i++) {
        uint32_t ack = reg(G.regs, IGPU_FORCEWAKE_ACK_GT) & IGPU_FORCEWAKE_KERNEL;

        if ((ack != 0) == on) {
            awake = on;
            return true;
        }

        if (i >= 5000) {
            kosmos_sleep(1);
        }
    }

    return false;
}

#define RING_PAGES   4u                 /* 16 KB: sixty-four copies and room */
#define RING_DWORDS  (RING_PAGES * 1024u)
#define FENCE_AT     64u                /* the status page's dword the fence lands in: byte 256 */
#define SCRATCH_AT   80u                /* a qword the invalidation's post-sync write lands in: byte 320 */
#define TS_AT        96u                /* the engine's clock before and after each copy: */
#define TS_SLOTS     16u                /*   two dwords a fence, sixteen fences round */
#define RUN_TS_AT    128u               /* a run's clock before and after: two dwords a run, */
#define RUN_TS_SLOTS 16u                /*   sixteen runs round, by fence, never zeroed */

/* Which fence each run slot was last written for, so the render engine's
 * busy time counts a run's clock readings once, and never a stale slot's. */
static uint32_t run_fence_at[RUN_TS_SLOTS];
#define RING_ROOM    (RING_DWORDS - 256u)   /* what may be in flight, a margin kept */

/*
 * **An engine with a ring** (`m700-2d.md` 5 and 7a): the blitter at
 * 0x22000 and the render engine at 0x2000, two of one thing - the same ring
 * registers at their own base, started the same way (i915's `xcs_resume`
 * is one function for every engine), each with its own ring, status page
 * and fence. Only the fence's command differs, MI_FLUSH_DW against
 * PIPE_CONTROL (`submit`).
 */
struct engine {
    uint32_t          base;             /* where its registers are */
    bool              render;           /* the render engine: its fence is PIPE_CONTROL */
    const char       *name;             /* as the log says it */
    bool              ready;
    volatile uint32_t *ring, *status, *target;
    uint32_t          ring_gtt, status_gtt, target_gtt;
    uint32_t          tail;             /* in dwords */
    uint32_t          next_value;       /* the engine test's */
    uint32_t          issued;           /* the last fence asked for */
    unsigned long     busy_since;       /* counter: when the work now running was given */
    unsigned long     busy_counter;     /* the counter's ticks the blitter has worked, in all */
    uint32_t          blits;            /* copies given it, since boot */
    unsigned long     ts_hz;            /* RING_TIMESTAMP's rate, measured against the counter */
    uint32_t          counted;          /* the last fence whose copy's time is added */
    uint64_t          busy_ts;          /* the engine's clock ticks spent copying, in all */
    uint32_t          in_flight;        /* dwords given since the ring was last idle */
};

static struct engine BCS = { .base = IGPU_BLT_BASE, .name = "the blitter" };
static struct engine RCS = { .base = IGPU_RENDER_BASE, .render = true, .name = "the render engine" };

static uint32_t eng(const struct engine *e, uint32_t offset)
{
    return reg(G.regs, e->base + offset);
}

static void eng_write(const struct engine *e, uint32_t offset, uint32_t value)
{
    wreg(e->base + offset, value);
}

static uint32_t engine_start(struct engine *e)
{
    unsigned i;

    if (e->ready) {
        return GPU_OK;
    }

    /* Awake first: register writes to a GT asleep in RC6 are lost, and the
     * first blit, which comes with no engine test before it, found the ring
     * refusing RING_VALID for exactly that (`testing.md` 18.667). */
    if (!forcewake(true)) {
        return GPU_ERR_NO_WAKE;
    }

    if (!gtt_pages(RING_PAGES, &e->ring, &e->ring_gtt) || !gtt_pages(1, &e->status, &e->status_gtt)
        || !gtt_pages(1, &e->target, &e->target_gtt)) {
        return GPU_ERR_NO_MEMORY;
    }

    /* stop_ring: emptied, disabled, reset. */
    eng_write(e, IGPU_RING_HEAD, eng(e, IGPU_RING_TAIL));
    (void)eng(e, IGPU_RING_HEAD);
    eng_write(e, IGPU_RING_CTL, 0);
    (void)eng(e, IGPU_RING_CTL);
    eng_write(e, IGPU_RING_HEAD, 0);
    eng_write(e, IGPU_RING_TAIL, 0);

    /* The status page, its writes all masked. */
    eng_write(e, IGPU_RING_HWS_PGA, e->status_gtt);
    (void)eng(e, IGPU_RING_HWS_PGA);
    eng_write(e, IGPU_RING_HWSTAM, ~0u);

    /* Then the ring itself: where, empty, its size, valid. */
    (void)eng(e, IGPU_RING_HEAD);
    eng_write(e, IGPU_RING_START, e->ring_gtt);
    eng_write(e, IGPU_RING_HEAD, 0);
    eng_write(e, IGPU_RING_TAIL, 0);
    (void)eng(e, IGPU_RING_TAIL);
    eng_write(e, IGPU_RING_CTL, ((RING_PAGES - 1u) << 12) | IGPU_RING_VALID);

    for (i = 0; i < 50 && (eng(e, IGPU_RING_CTL) & IGPU_RING_VALID) == 0; i++) {
        kosmos_sleep(1);
    }

    if ((eng(e, IGPU_RING_CTL) & IGPU_RING_VALID) == 0) {
        return GPU_ERR_NO_RING;
    }

    eng_write(e, IGPU_RING_MI_MODE, IGPU_STOP_RING << 16);     /* masked: STOP_RING off */

    /*
     * **The engine's clock's rate, measured** rather than worked out from
     * CTC_MODE (`intel_gt_clock_utils.c`: 24 MHz on Skylake, divided by a
     * shift the firmware chose): RING_TIMESTAMP read across five scheduler
     * ticks against the processor's counter, whose rate the kernel knows.
     */
    {
        static struct sysinfo machine;     /* large: not on the stack */
        uint32_t ts0, ts1;
        unsigned long c0, c1;

        ts0 = eng(e, IGPU_RING_TIMESTAMP);
        c0 = kosmos_ticks();
        kosmos_sleep(5);
        ts1 = eng(e, IGPU_RING_TIMESTAMP);
        c1 = kosmos_ticks();

        if (kosmos_sysinfo(&machine) == 0 && c1 > c0 && ts1 != ts0) {
            e->ts_hz = (unsigned long)((uint64_t)(uint32_t)(ts1 - ts0) * machine.counter_hz / (c1 - c0));
        }
    }

    e->tail = 0;
    e->status[FENCE_AT] = 0;
    e->issued = 0;
    e->next_value = 0x4b6f7301u;                             /* "Kos" and a count */
    e->ready = true;
    return GPU_OK;
}

/* The fence the blitter has reached. */
static uint32_t fence_reached(const struct engine *e)
{
    return e->status[FENCE_AT];
}

/* Whether everything given has been done - and if so the GT let go, and
 * the time it worked added to its count. */
static bool engine_quiet(const struct engine *e)
{
    return !e->ready || fence_reached(e) == e->issued;
}

/* Whether everything given this engine has been done - and the GT let go
 * when neither engine has work, since one forcewake serves both. */
static bool engine_idle(struct engine *e)
{
    /* The copies finished since: each one's time, by the engine's own
     * clock, added - fences the engine test made store no times and are
     * skipped by having their slot zeroed. */
    if (e->ready) {
        uint32_t reached = fence_reached(e);

        while ((int32_t)(reached - e->counted) > 0) {
            volatile uint32_t *t = &e->status[TS_AT + 2u * (++e->counted % TS_SLOTS)];

            e->busy_ts += (uint32_t)(t[1] - t[0]);
            t[0] = t[1] = 0;

            /* And on the render engine, a program's run: its clock before
             * its first thread and after its last (GPU_OP_RUN_TIME's). */
            if (e->render && run_fence_at[e->counted % RUN_TS_SLOTS] == e->counted) {
                volatile uint32_t *r = &e->status[RUN_TS_AT + 2u * (e->counted % RUN_TS_SLOTS)];

                e->busy_ts += (uint32_t)(r[1] - r[0]);
            }
        }
    }

    if (engine_quiet(e)) {
        e->in_flight = 0;
        if (e->busy_since != 0) {
            e->busy_counter += kosmos_ticks() - e->busy_since;
            e->busy_since = 0;
        }

        if (engine_quiet(&BCS) && engine_quiet(&RCS)) {
            (void)forcewake(false);
        }

        return true;
    }

    return false;
}

/*
 * Commands into the ring: `n` dwords at the tail, the rest of the ring
 * filled with MI_NOOP and the tail back at its start when they would not
 * fit before its end - the engine reads them in order and wraps as the
 * tail did. What is written since the ring was last idle is counted, and
 * a submission refused (`GPU_ERR_BUSY`) before it could reach the head.
 */
static void emit(struct engine *e, const uint32_t *dw, uint32_t n)
{
    uint32_t i;

    if (e->tail + n > RING_DWORDS) {
        e->in_flight += RING_DWORDS - e->tail;

        while (e->tail < RING_DWORDS) {
            e->ring[e->tail++] = IGPU_MI_NOOP;
        }

        e->tail = 0;
    }

    e->in_flight += n;

    for (i = 0; i < n; i++) {
        e->ring[e->tail++] = dw[i];
    }

    e->tail %= RING_DWORDS;
}

/* The fence after them, and the doorbell: the tail moved, with the GT
 * awake. Answers the fence to look for. */
static uint32_t submit(struct engine *e)
{
    uint32_t fence = ++e->issued;

    if (e->render) {
        /*
         * **As i915 ends a request on the render engine**
         * (`gen8_emit_fini_breadcrumb_rcs`): the caches flushed - render
         * target, depth, data port, and the TLB invalidated - and then the
         * fence, PIPE_CONTROL's post-sync write of a qword at FENCE_AT (byte
         * 256, 8-aligned, the dword after it written over), with
         * FLUSH_ENABLE so it waits for those flushes to finish. Without it
         * the fence was written while a frame's pixels were still on their
         * way out of the render cache, and the copy to the screen was read
         * part old: 2% of its pixels, a channel or two each (7d).
         */
        uint32_t flush[6] = {
            IGPU_PIPE_CONTROL,
            IGPU_PC_CS_STALL | IGPU_PC_TLB_INVALIDATE | IGPU_PC_RT_FLUSH | IGPU_PC_DEPTH_FLUSH
                | IGPU_PC_DC_FLUSH,
            0, 0, 0, 0,
        };
        uint32_t dw[6] = {
            IGPU_PIPE_CONTROL,
            IGPU_PC_FLUSH_ENABLE | IGPU_PC_CS_STALL | IGPU_PC_QW_WRITE | IGPU_PC_GLOBAL_GTT,
            e->status_gtt + FENCE_AT * 4u,
            0,
            fence,
            0,
        };

        emit(e, flush, 6);
        emit(e, dw, 6);
    } else {
        uint32_t dw[4] = {
            IGPU_MI_FLUSH_DW_STORE,
            (e->status_gtt + FENCE_AT * 4u) | IGPU_MI_FLUSH_DW_GTT,
            0,
            fence,
        };

        emit(e, dw, 4);
    }

    if ((e->tail & 1u) != 0) {               /* the tail is in qwords */
        uint32_t noop = IGPU_MI_NOOP;
        emit(e, &noop, 1);
    }

#if defined(__x86_64__)
    __asm__ volatile("sfence" ::: "memory");    /* the ring's writes out before the doorbell */
#endif

    (void)forcewake(true);
    e->busy_since = kosmos_ticks();
    eng_write(e, IGPU_RING_TAIL, e->tail * 4u);
    return fence;
}

static void engine_test(long console, struct engine *e, struct gpu_reply *rep)
{
    struct say_line line;
    uint32_t value = 0, error;
    unsigned long t0, t1 = 0;
    unsigned i;

    if (!G.found) {
        rep->error = GPU_ERR_NO_DEVICE;
        return;
    }

    if (!forcewake(true)) {
        rep->error = GPU_ERR_NO_WAKE;
        return;
    }

    error = engine_start(e);

    if (error == GPU_OK && !engine_idle(e)) {
        error = GPU_ERR_BUSY;
    }

    if (error == GPU_OK) {
        uint32_t dw[4];

        value = e->next_value++;
        e->target[0] = 0;
        dw[0] = IGPU_MI_STORE_DWORD | IGPU_MI_USE_GGTT;
        dw[1] = e->target_gtt;
        dw[2] = 0;
        dw[3] = value;
        emit(e, dw, 4);

        t0 = kosmos_ticks();
        (void)submit(e);

        /* Looked for without sleeping first, a few thousand reads, so the
         * time said is the GPU's rather than a scheduler tick's; then a
         * tick at a time, up to a hundred. */
        for (i = 0; i < 20000 && e->target[0] != value; i++) {
            __asm__ volatile("" ::: "memory");
        }

        for (i = 0; i < 100 && e->target[0] != value; i++) {
            kosmos_sleep(1);
        }

        t1 = kosmos_ticks();
        rep->engine.value = e->target[0];

        {
            static struct sysinfo machine;     /* large: not on the stack */
            unsigned long per_us;

            per_us = kosmos_sysinfo(&machine) == 0 && machine.counter_hz >= 1000000u
                   ? machine.counter_hz / 1000000u : 1u;
            rep->engine.elapsed_us = (uint32_t)((t1 - t0) / per_us);
        }

        if (e->target[0] != value) {
            error = GPU_ERR_NO_ANSWER;
        }
    }

    (void)forcewake(true);
    rep->engine.head = eng(e, IGPU_RING_HEAD);
    rep->engine.tail = eng(e, IGPU_RING_TAIL);
    rep->engine.ctl = eng(e, IGPU_RING_CTL);
    rep->engine.mode = eng(e, IGPU_RING_MODE);
    rep->engine.esr = eng(e, IGPU_RING_ESR);
    rep->error = error;

    for (i = 0; i < 100 && !engine_idle(e); i++) {
        kosmos_sleep(1);
    }

    say_begin(&line);
    say_text(&line, "intelgpu: ");
    say_text(&line, e->name);
    say_text(&line, error == GPU_OK ? " wrote " : " did not write; read ");
    say_hex(&line, rep->engine.value, 8);
    say_text(&line, ", ring at GTT ");
    say_hex(&line, e->ring_gtt, 8);
    say_text(&line, ", head ");
    say_hex(&line, rep->engine.head, 8);
    say_text(&line, " tail ");
    say_hex(&line, rep->engine.tail, 8);
    say_text(&line, " ctl ");
    say_hex(&line, rep->engine.ctl, 8);
    say_text(&line, " mode ");
    say_hex(&line, rep->engine.mode, 8);
    say_text(&line, " esr ");
    say_hex(&line, rep->engine.esr, 8);
    say_send(console, &line);
}

/*
 * **The first program on the execution units** (`m700-2d.md` 7b): the
 * render engine switched to its GPGPU pipeline, given a kernel, and walked
 * over sixteen thread groups - four across, four down - each filling 16
 * bytes of a 64 by 4 surface with `colour` (`intelgpu_fill.h`). Set up as
 * intel-gpu-tools sets up its fill for Skylake (`lib/gpu_cmds.c`): the
 * state in one page that is the base of the surface, dynamic and
 * instruction state - the kernel, its constants, the interface descriptor,
 * the binding table and the surface - and the commands in the ring itself.
 * Answers how many of the 256 bytes are the colour, and the 256 after them
 * are held to zero, so a walk past the surface is seen.
 */
#define CS_KERNEL_AT  0x000u
#define CS_CURBE_AT   0x100u
#define CS_IDD_AT     0x140u
#define CS_BT_AT      0x180u
#define CS_SS_AT      0x1c0u
#define CS_W          64u                /* the surface, in bytes */
#define CS_H          4u                 /*   and rows */

static volatile uint32_t *cs_state, *cs_target;
static uint32_t cs_state_gtt, cs_target_gtt;

static void compute_test(long console, uint32_t colour, struct gpu_reply *rep)
{
    struct say_line line;
    volatile uint8_t *bytes;
    uint32_t error, right = 0, past = 0;
    unsigned long t0, t1;
    unsigned i;

    if (!G.found) {
        rep->error = GPU_ERR_NO_DEVICE;
        return;
    }

    if (!forcewake(true)) {
        rep->error = GPU_ERR_NO_WAKE;
        return;
    }

    error = engine_start(&RCS);

    if (error == GPU_OK && !engine_idle(&RCS)) {
        error = GPU_ERR_BUSY;
    }

    if (error == GPU_OK && cs_state == NULL
        && (!gtt_pages(1, &cs_state, &cs_state_gtt) || !gtt_pages(1, &cs_target, &cs_target_gtt))) {
        error = GPU_ERR_NO_MEMORY;
    }

    if (error == GPU_OK) {
        volatile uint32_t *k = cs_state + CS_KERNEL_AT / 4u;
        volatile uint32_t *idd = cs_state + CS_IDD_AT / 4u;
        volatile uint32_t *ss = cs_state + CS_SS_AT / 4u;
        const uint32_t *words = &intelgpu_fill_kernel[0][0];
        uint32_t sba_lo = cs_state_gtt | 1u;            /* the base, and modify */

        for (i = 0; i < 1024u; i++) {
            cs_state[i] = 0;
            cs_target[i] = 0;
        }

        for (i = 0; i < sizeof(intelgpu_fill_kernel) / 4u; i++) {
            k[i] = words[i];
        }

        cs_state[CS_CURBE_AT / 4u] = colour & 0xffu;    /* r1's first byte */

        idd[0] = CS_KERNEL_AT;                          /* from the instruction base */
        idd[2] = 1u << 18;                              /* single program flow, IEEE */
        idd[4] = CS_BT_AT;                              /* from the surface base, 32-aligned */
        idd[5] = 1u << 16;                              /* one register of constants: r1 */
        idd[6] = 1u;                                    /* one thread a group */

        cs_state[CS_BT_AT / 4u] = CS_SS_AT;             /* surface 0 */

        ss[0] = (1u << 29) | (IGPU_SURFACEFORMAT_R8_UNORM << 18) | (1u << 16) | (1u << 14) | (1u << 8);
        ss[2] = (CS_W - 1u) | ((CS_H - 1u) << 16);
        ss[3] = CS_W - 1u;                              /* the pitch, less one */
        ss[7] = (4u << 25) | (5u << 22) | (6u << 19) | (7u << 16);   /* R, G, B, A */
        ss[8] = cs_target_gtt;

        {
            const uint32_t before[6] = { IGPU_PIPE_CONTROL, IGPU_PC_CS_STALL | IGPU_PC_INVALIDATE, 0, 0, 0, 0 };
            const uint32_t select = IGPU_PIPELINE_SELECT;
            const uint32_t sba[19] = {
                IGPU_STATE_BASE_ADDRESS,
                1u, 0,                                  /* general: 0, modified */
                1u,                                     /* stateless data port */
                sba_lo, 0,                              /* surface state */
                sba_lo, 0,                              /* dynamic state */
                0, 0,                                   /* indirect objects */
                sba_lo, 0,                              /* instructions */
                0xfffff000u | 1u, 0x1000u | 1u,         /* sizes: general, dynamic - a page */
                0xfffff000u | 1u, 0x1000u | 1u,         /*   indirect, instructions - a page */
                1u, 0, 0xfffff000u,                     /* bindless surface state */
            };
            const uint32_t vfe[9] = { IGPU_MEDIA_VFE_STATE, 0, 0, (1u << 16) | (1u << 8), 0, 1u, 0, 0, 0 };
            const uint32_t curbe[4] = { IGPU_MEDIA_CURBE_LOAD, 0, 64u, CS_CURBE_AT };
            const uint32_t idl[4] = { IGPU_MEDIA_IDL, 0, 32u, CS_IDD_AT };
            const uint32_t walk[15] = {
                IGPU_GPGPU_WALKER, 0, 0, 0,
                1u << 30,                               /* SIMD16, a thread a group */
                0, 0, CS_W / 16u,                       /* groups across */
                0, 0, CS_H,                             /*   and down */
                0, 1u,
                0xffffu, 0xffffffffu,                   /* every channel right and bottom */
            };
            const uint32_t flush[2] = { IGPU_MEDIA_STATE_FLUSH, 0 };
            const uint32_t after[6] = { IGPU_PIPE_CONTROL, IGPU_PC_CS_STALL | IGPU_PC_DC_FLUSH | IGPU_PC_RT_FLUSH,
                                        0, 0, 0, 0 };

            emit(&RCS, before, 6);
            emit(&RCS, &select, 1);
            emit(&RCS, sba, 19);
            emit(&RCS, vfe, 9);
            emit(&RCS, curbe, 4);
            emit(&RCS, idl, 4);
            emit(&RCS, walk, 15);
            emit(&RCS, flush, 2);
            emit(&RCS, after, 6);
        }

#if defined(__x86_64__)
        __asm__ volatile("sfence" ::: "memory");    /* the state's writes out before the doorbell */
#endif

        t0 = kosmos_ticks();
        {
            uint32_t fence = submit(&RCS);

            for (i = 0; i < 20000 && fence_reached(&RCS) != fence; i++) {
                __asm__ volatile("" ::: "memory");
            }

            for (i = 0; i < 100 && fence_reached(&RCS) != fence; i++) {
                kosmos_sleep(1);
            }

            if (fence_reached(&RCS) != fence) {
                error = GPU_ERR_NO_ANSWER;
            }
        }
        t1 = kosmos_ticks();

        bytes = (volatile uint8_t *)cs_target;

        for (i = 0; i < CS_W * CS_H; i++) {
            right += bytes[i] == (uint8_t)colour;
        }

        for (i = CS_W * CS_H; i < 2u * CS_W * CS_H; i++) {
            past += bytes[i] != 0;
        }

        rep->engine.value = right;
        rep->engine.ctl = past;

        {
            static struct sysinfo machine;     /* large: not on the stack */
            unsigned long per_us;

            per_us = kosmos_sysinfo(&machine) == 0 && machine.counter_hz >= 1000000u
                   ? machine.counter_hz / 1000000u : 1u;
            rep->engine.elapsed_us = (uint32_t)((t1 - t0) / per_us);
        }

        if (error == GPU_OK && (right != CS_W * CS_H || past != 0)) {
            error = GPU_ERR_UNCHANGED;
        }
    }

    (void)forcewake(true);
    rep->engine.head = eng(&RCS, IGPU_RING_HEAD);
    rep->engine.tail = eng(&RCS, IGPU_RING_TAIL);
    rep->engine.mode = eng(&RCS, IGPU_RING_MODE);
    rep->engine.esr = eng(&RCS, IGPU_RING_ESR);
    rep->error = error;

    for (i = 0; i < 100 && !engine_idle(&RCS); i++) {
        kosmos_sleep(1);
    }

    say_begin(&line);
    say_text(&line, "intelgpu: the execution units ");
    say_text(&line, error == GPU_OK ? "filled " : "did not fill; ");
    say_dec(&line, right);
    say_text(&line, " of 256 bytes with ");
    say_hex(&line, colour & 0xffu, 2);
    say_text(&line, ", ");
    say_dec(&line, past);
    say_text(&line, " written past; head ");
    say_hex(&line, rep->engine.head, 8);
    say_text(&line, " tail ");
    say_hex(&line, rep->engine.tail, 8);
    say_text(&line, " esr ");
    say_hex(&line, rep->engine.esr, 8);
    say_send(console, &line);
}

/*
 * **A program's own kernels** (`docs/compute.md` C2): a kernel's
 * instructions copied out of the region they came in, checked there
 * (`intelgpu_kernel.c`, decision D2) - so nothing the program does to its
 * region after can change what was checked - and copied again into pages
 * of the driver's own, which are the instruction base of every run of it.
 * A kernel let go keeps its pages for the next one that fits, since GTT
 * slots are never given back.
 */
#define KERNELS_MAX   64u

static struct kernel {
    bool               used;
    uint32_t           holder;          /* the process that handed it in */
    volatile uint32_t *cpu;
    uint32_t           gtt;             /* its pages' GTT address: a run's instruction base */
    uint32_t           pages, bytes;
    int32_t            bti_max;         /* a run must bind at least this many buffers, plus one */
} K[KERNELS_MAX];

static uint32_t kernel_words[IGPU_KERNEL_MAX_BYTES / 4u];

static uint32_t kernel_new(long console, long cap, const struct gpu_request *rq, struct gpu_reply *rep)
{
    struct igpu_kernel_verdict v;
    struct say_line line;
    uint32_t bytes = (uint32_t)rq->x, pages = ((uint32_t)rq->x + 4095u) / 4096u, id, i, spare = KERNELS_MAX;
    long size, at;

    if (!G.found) {
        return GPU_ERR_NO_DEVICE;
    }

    if (cap < 0 || rq->x <= 0 || bytes > IGPU_KERNEL_MAX_BYTES || (size = kosmos_mem_size(cap)) <= 0
        || (unsigned long)rq->offset + bytes > (unsigned long)size * 4096ul
        || (at = kosmos_mem_map(cap)) < 0) {
        return GPU_ERR_BAD_OP;
    }

    __builtin_memcpy(kernel_words, (const char *)at + rq->offset, bytes);
    kosmos_share_unmap((unsigned long)at, (unsigned long)size);

    if (igpu_kernel_check(kernel_words, bytes, &v) != GPU_KERNEL_OK) {
        rep->words[1] = v.at;
        rep->words[2] = v.why;
        return GPU_ERR_KERNEL;
    }

    /* A free slot - one whose pages are large enough first. */
    for (id = 1; id < KERNELS_MAX; id++) {
        if (!K[id].used && K[id].cpu != NULL && K[id].pages >= pages) {
            break;
        }

        if (!K[id].used && K[id].cpu == NULL && spare == KERNELS_MAX) {
            spare = id;
        }
    }

    if (id == KERNELS_MAX) {
        if (spare == KERNELS_MAX) {
            return GPU_ERR_NO_MEMORY;
        }

        id = spare;

        if (!gtt_pages(pages, &K[id].cpu, &K[id].gtt)) {
            K[id].cpu = NULL;
            return GPU_ERR_NO_MEMORY;
        }

        K[id].pages = pages;
    }

    for (i = 0; i < K[id].pages * 1024u; i++) {
        K[id].cpu[i] = i < bytes / 4u ? kernel_words[i] : 0u;
    }

    K[id].used = true;
    K[id].holder = holder_of_request();
    K[id].bytes = bytes;
    K[id].bti_max = v.bti_max;
    rep->buffer.id = id;
    rep->buffer.gtt = K[id].gtt;
    rep->buffer.pages = K[id].pages;

    say_begin(&line);
    say_text(&line, "intelgpu: kernel ");
    say_dec(&line, id);
    say_text(&line, " checked and kept, ");
    say_dec(&line, bytes / 16u);
    say_text(&line, " instructions, buffers up to ");
    say_dec(&line, (unsigned long)(v.bti_max + 1));
    say_send(console, &line);
    return GPU_OK;
}

static uint32_t kernel_forget(const struct gpu_request *rq)
{
    uint32_t id = (uint32_t)rq->x;

    if (rq->x <= 0 || id >= KERNELS_MAX || !K[id].used) {
        return GPU_ERR_BAD_OP;
    }

    if (K[id].holder != holder_of_request()) {
        return GPU_ERR_NOT_YOURS;
    }

    if (!engine_idle(&RCS)) {
        return GPU_ERR_BUSY;            /* a run of it may still be reading it */
    }

    K[id].used = false;
    K[id].holder = 0;
    return GPU_OK;
}

/*
 * **A run** (`compute.md` C2): the kernel over a grid of thread groups, a
 * thread a group, as 7b's fill - `PIPELINE_SELECT` GPGPU, the bases, the
 * front end, the constants, the descriptor, the walker - with the kernel's
 * own pages as the instruction base and the buffers it is handed in its
 * binding table, each a surface the driver makes from what the buffer
 * is: the hardware drops a message's reads and writes outside a surface,
 * so the buffers are all a kernel reaches. Answered at once with the
 * fence; one run at a time, since its state page is one.
 *
 * Its state page: the constants at 0, the descriptor at 0x100, the binding
 * table at 0x140 and eight surface states from 0x180, 64 bytes each. The
 * buffers are seen uncached (MOCS 0), so what the processor wrote is what
 * the kernel reads and what it writes is in memory when the fence comes.
 */
#define RUN_CURBE_AT  0x000u
#define RUN_IDD_AT    0x100u
#define RUN_BT_AT     0x140u
#define RUN_SS_AT     0x180u
#define RUN_THREADS   168u              /* the HD 530's 24 EUs, seven threads each */

#define SURFACEFORMAT_B8G8R8A8_UNORM 0x0c0u   /* isl_format.h */
#define SURFACEFORMAT_R32_FLOAT      0x0d8u
#define SURFACEFORMAT_RAW            0x1ffu

static volatile uint32_t *run_state;
static uint32_t run_state_gtt, run_fence;
static bool run_given;

/* A buffer as its kernel sees it, into 16 dwords of surface state. */
static bool run_surface(volatile uint32_t *ss, const struct buffer *b, uint32_t view)
{
    unsigned i;

    for (i = 0; i < 16; i++) {
        ss[i] = 0;
    }

    if (view == GPU_VIEW_BYTES) {
        uint32_t last = b->pitch * b->height - 1u;      /* elements of one byte, less one */

        ss[0] = (4u << 29) | (SURFACEFORMAT_RAW << 18) | (1u << 8);
        ss[2] = (last & 0x7fu) | (((last >> 7) & 0x3fffu) << 16);
        ss[3] = ((last >> 21) & 0x7ffu) << 21;           /* the pitch: one byte, less one */
    } else {
        uint32_t format, width;

        switch (view) {
        case GPU_VIEW_BGRA: format = SURFACEFORMAT_B8G8R8A8_UNORM; width = b->width;      break;
        case GPU_VIEW_R8:   format = IGPU_SURFACEFORMAT_R8_UNORM;  width = b->width * 4u; break;
        case GPU_VIEW_R32F: format = SURFACEFORMAT_R32_FLOAT;      width = b->width;      break;
        default:            return false;
        }

        ss[0] = (1u << 29) | (format << 18) | (1u << 16) | (1u << 14) | (1u << 8);
        ss[2] = (width - 1u) | ((b->height - 1u) << 16);
        ss[3] = b->pitch - 1u;
    }

    ss[7] = (4u << 25) | (5u << 22) | (6u << 19) | (7u << 16);   /* R, G, B, A */
    ss[8] = b->gtt;
    return true;
}

static uint32_t run(const struct gpu_run *q, struct gpu_reply *rep)
{
    const uint32_t who = holder_of_request();
    const struct kernel *k;
    uint32_t error, i, regs;

    if (!G.found) {
        return GPU_ERR_NO_DEVICE;
    }

    if (q->kernel == 0 || q->kernel >= KERNELS_MAX || !K[q->kernel].used
        || (q->simd != 8u && q->simd != 16u) || q->groups_x == 0 || q->groups_y == 0
        || q->groups_x > 65535u || q->groups_y > 65535u || q->nbuffers > GPU_RUN_BUFFERS
        || q->nconstants > GPU_RUN_CONSTANTS || q->nconstants % 32u != 0) {
        return GPU_ERR_BAD_OP;
    }

    k = &K[q->kernel];

    if (k->holder != who || who == 0) {
        return GPU_ERR_NOT_YOURS;
    }

    if ((int32_t)q->nbuffers <= k->bti_max) {
        return GPU_ERR_BAD_OP;          /* the kernel names an entry this run does not bind */
    }

    /* Each buffer imported, and imported by this program: never a screen,
     * never another program's picture. */
    for (i = 0; i < q->nbuffers; i++) {
        uint32_t id = q->buffers[i].id;

        if (id == 0 || id >= BUFFERS_MAX || !B[id].used || B[id].kind != GPU_BUFFER_IMPORTED
            || q->buffers[i].view > GPU_VIEW_R32F) {
            return GPU_ERR_NO_BUFFER;
        }

        if (B[id].holder != who) {
            return GPU_ERR_NOT_YOURS;
        }
    }

    if (!forcewake(true)) {
        return GPU_ERR_NO_WAKE;
    }

    if ((error = engine_start(&RCS)) != GPU_OK) {
        return error;
    }

    if (run_state == NULL && !gtt_pages(1, &run_state, &run_state_gtt)) {
        run_state = NULL;
        return GPU_ERR_NO_MEMORY;
    }

    if ((run_given && (int32_t)(fence_reached(&RCS) - run_fence) < 0) || RCS.in_flight + 112u > RING_ROOM) {
        (void)engine_idle(&RCS);
        return GPU_ERR_BUSY;
    }

    for (i = 0; i < 1024u; i++) {
        run_state[i] = 0;
    }

    for (i = 0; i < q->nconstants / 4u; i++) {
        uint32_t w;

        __builtin_memcpy(&w, &q->constants[i * 4u], 4);
        run_state[RUN_CURBE_AT / 4u + i] = w;
    }

    for (i = 0; i < q->nbuffers; i++) {
        (void)run_surface(run_state + (RUN_SS_AT + i * 64u) / 4u, &B[q->buffers[i].id], q->buffers[i].view);
        run_state[RUN_BT_AT / 4u + i] = RUN_SS_AT + i * 64u;
    }

    regs = q->nconstants / 32u;

    {
        volatile uint32_t *idd = run_state + RUN_IDD_AT / 4u;
        const uint32_t sba_lo = run_state_gtt | 1u;     /* the base, and modify */

        idd[0] = 0;                                     /* the kernel, from the instruction base */
        idd[2] = 1u << 18;                              /* single program flow, IEEE */
        idd[4] = RUN_BT_AT;                             /* from the surface base, 32-aligned */
        idd[5] = regs << 16;                            /* registers of constants: r1 onwards */
        idd[6] = 1u;                                    /* one thread a group */

        {
            const uint32_t before[6] = { IGPU_PIPE_CONTROL, IGPU_PC_CS_STALL | IGPU_PC_INVALIDATE, 0, 0, 0, 0 };
            const uint32_t select = IGPU_PIPELINE_SELECT;
            const uint32_t sba[19] = {
                IGPU_STATE_BASE_ADDRESS,
                1u, 0,                                  /* general: 0, modified */
                1u,                                     /* stateless data port: no message reaches it */
                sba_lo, 0,                              /* surface state */
                sba_lo, 0,                              /* dynamic state */
                0, 0,                                   /* indirect objects */
                k->gtt | 1u, 0,                         /* instructions: the kernel's own pages */
                0xfffff000u | 1u, 0x1000u | 1u,         /* sizes: general, dynamic - a page */
                0xfffff000u | 1u, (k->pages << 12) | 1u,/*   indirect, instructions - the kernel's */
                1u, 0, 0xfffff000u,                     /* bindless surface state */
            };
            const uint32_t vfe[9] = {
                IGPU_MEDIA_VFE_STATE, 0, 0,
                ((RUN_THREADS - 1u) << 16) | (1u << 8),     /* threads at once, less one; URB entries */
                0,
                regs > 0 ? regs : 1u,                       /* the constants' share, in registers */
                0, 0, 0,
            };
            const uint32_t curbe[4] = { IGPU_MEDIA_CURBE_LOAD, 0, q->nconstants, RUN_CURBE_AT };
            const uint32_t idl[4] = { IGPU_MEDIA_IDL, 0, 32u, RUN_IDD_AT };
            const uint32_t walk[15] = {
                IGPU_GPGPU_WALKER, 0, 0, 0,
                q->simd == 16u ? 1u << 30 : 0u,         /* SIMD16 or SIMD8, a thread a group */
                0, 0, q->groups_x,                      /* groups across */
                0, 0, q->groups_y,                      /*   and down */
                0, 1u,
                q->simd == 16u ? 0xffffu : 0xffu,       /* every channel, right and bottom */
                0xffffffffu,
            };
            const uint32_t flush[2] = { IGPU_MEDIA_STATE_FLUSH, 0 };

            emit(&RCS, before, 6);
            emit(&RCS, &select, 1);
            emit(&RCS, sba, 19);
            emit(&RCS, vfe, 9);

            if (regs > 0) {
                emit(&RCS, curbe, 4);
            }

            /* The engine's clock around the walker - the second after a
             * stall, so after the last thread has ended - into the run's
             * slot, kept for GPU_OP_RUN_TIME. */
            const uint32_t slot = RUN_TS_AT + 2u * ((RCS.issued + 1u) % RUN_TS_SLOTS);
            const uint32_t t0[4] = { IGPU_MI_STORE_REG_MEM, IGPU_RENDER_BASE + IGPU_RING_TIMESTAMP,
                                     RCS.status_gtt + slot * 4u, 0 };
            const uint32_t stall[6] = { IGPU_PIPE_CONTROL, IGPU_PC_CS_STALL, 0, 0, 0, 0 };
            const uint32_t t1[4] = { IGPU_MI_STORE_REG_MEM, IGPU_RENDER_BASE + IGPU_RING_TIMESTAMP,
                                     RCS.status_gtt + (slot + 1u) * 4u, 0 };

            emit(&RCS, idl, 4);
            emit(&RCS, t0, 4);
            emit(&RCS, walk, 15);
            emit(&RCS, flush, 2);
            emit(&RCS, stall, 6);
            emit(&RCS, t1, 4);
        }
    }

#if defined(__x86_64__)
    __asm__ volatile("sfence" ::: "memory");    /* the state's writes out before the doorbell */
#endif

    run_fence_at[(RCS.issued + 1u) % RUN_TS_SLOTS] = RCS.issued + 1u;
    run_fence = submit(&RCS);
    run_given = true;
    rep->engine.value = run_fence;
    return GPU_OK;
}

/*
 * **What a run cost the GPU** (`compute.md` C3): its two clock readings,
 * by its fence, in nanoseconds - `engine.value` the low word and
 * `engine.head` the high. Busy until the fence is reached; refused for a
 * run more than sixteen ago, whose slot has been written again.
 */
static uint32_t run_time(const struct gpu_request *rq, struct gpu_reply *rep)
{
    uint32_t fence = (uint32_t)rq->x, slot, ticks;
    uint64_t ns;

    if (!G.found) {
        return GPU_ERR_NO_DEVICE;
    }

    if (!RCS.ready || RCS.ts_hz == 0 || fence == 0 || (int32_t)(RCS.issued - fence) < 0) {
        return GPU_ERR_BAD_OP;
    }

    if ((int32_t)(fence_reached(&RCS) - fence) < 0) {
        (void)engine_idle(&RCS);
        return GPU_ERR_BUSY;
    }

    if (RCS.issued - fence >= RUN_TS_SLOTS) {
        return GPU_ERR_NO_ANSWER;
    }

    slot = RUN_TS_AT + 2u * (fence % RUN_TS_SLOTS);
    ticks = RCS.status[slot + 1u] - RCS.status[slot];
    ns = (uint64_t)ticks * 1000000000ull / RCS.ts_hz;
    rep->engine.value = (uint32_t)ns;
    rep->engine.head = (uint32_t)(ns >> 32);
    rep->engine.tail = ticks;
    return GPU_OK;
}

/*
 * **The render engine's starting state** (`m700-2d.md` 7c): i915's batch
 * for a new context (`intelgpu_renderstate.h`), placed as
 * `render_state_setup` places it - each relocation the batch's own address
 * plus the offset it holds - and run from the ring once. Kosmos gives the
 * render engine no context, so this is what i915 would have run first.
 */
static volatile uint32_t *rs_batch;
static uint32_t rs_batch_gtt;
static bool rs_done;

/*
 * **The render engine's cache controls, as i915 leaves them** (7d): every
 * MOCS field in the batches names index 1, "as the page table says", which
 * is what the blitter does without being told - and a reset does not leave
 * index 1 so. Said before and after, once.
 */
static bool mocs_done;

static void mocs_program(long console)
{
    struct say_line line;
    uint32_t i, before0 = reg(G.regs, IGPU_RCS_MOCS0), before1 = reg(G.regs, IGPU_RCS_MOCS0 + 4u),
             l3before = reg(G.regs, IGPU_LNCFCMOCS0);

    for (i = 0; i < 62u; i++) {
        uint32_t v = i == 0 ? IGPU_MOCS_UNCACHED : i == 2 ? IGPU_MOCS_CACHED : IGPU_MOCS_PTE;

        wreg(IGPU_RCS_MOCS0 + i * 4u, v);
    }

    for (i = 0; i < 31u; i++) {
        /* Entry 2's L3 half - the low half of the second register - kept
         * out of the GPU's L3 (7d experiment): it is the sampler's, and the
         * L3 is not kept in step with the processor's writes. */
        uint32_t lo = i == 0 || i == 1 ? IGPU_L3_UC : IGPU_L3_WB;
        uint32_t hi = IGPU_L3_WB;

        wreg(IGPU_LNCFCMOCS0 + i * 4u, lo | hi << 16);
    }

    mocs_done = true;
    say_begin(&line);
    say_text(&line, "intelgpu: the render engine's MOCS programmed as i915's; index 0 was ");
    say_hex(&line, before0, 8);
    say_text(&line, ", 1 ");
    say_hex(&line, before1, 8);
    say_text(&line, ", L3 ");
    say_hex(&line, l3before, 8);
    say_text(&line, "; now ");
    say_hex(&line, reg(G.regs, IGPU_RCS_MOCS0), 8);
    say_text(&line, ", ");
    say_hex(&line, reg(G.regs, IGPU_RCS_MOCS0 + 4u), 8);
    say_text(&line, ", L3 ");
    say_hex(&line, reg(G.regs, IGPU_LNCFCMOCS0), 8);
    say_send(console, &line);
}

static long mocs_console = -1;

static uint32_t render_state(void)
{
    uint32_t error, fence, i, r = 0;
    const uint32_t n = sizeof(intelgpu_renderstate) / 4u;

    if (rs_done) {
        return GPU_OK;
    }

    if (!mocs_done) {
        mocs_program(mocs_console);
    }

    if ((error = engine_start(&RCS)) != GPU_OK) {
        return error;
    }

    if (!engine_idle(&RCS)) {
        return GPU_ERR_BUSY;
    }

    if (rs_batch == NULL && !gtt_pages(1, &rs_batch, &rs_batch_gtt)) {
        return GPU_ERR_NO_MEMORY;
    }

    for (i = 0; i < n; i++) {
        uint32_t w = intelgpu_renderstate[i];

        if (r < sizeof(intelgpu_renderstate_relocs) / 4u && i * 4u == intelgpu_renderstate_relocs[r]) {
            w += rs_batch_gtt;                  /* the high half after it stays 0 */
            r++;
        }

        rs_batch[i] = w;
    }

    {
        const uint32_t start[3] = { IGPU_MI_BB_START, rs_batch_gtt, 0 };

#if defined(__x86_64__)
        __asm__ volatile("sfence" ::: "memory");
#endif
        emit(&RCS, start, 3);
    }

    fence = submit(&RCS);

    for (i = 0; i < 20000 && fence_reached(&RCS) != fence; i++) {
        __asm__ volatile("" ::: "memory");
    }

    for (i = 0; i < 100 && fence_reached(&RCS) != fence; i++) {
        kosmos_sleep(1);
    }

    if (fence_reached(&RCS) != fence) {
        return GPU_ERR_NO_ANSWER;
    }

    rs_done = true;
    return GPU_OK;
}

static void render_state_test(long console, struct gpu_reply *rep)
{
    struct say_line line;
    unsigned long t0 = kosmos_ticks(), t1;
    bool before = rs_done;
    unsigned i;

    if (!G.found) {
        rep->error = GPU_ERR_NO_DEVICE;
        return;
    }

    if (!forcewake(true)) {
        rep->error = GPU_ERR_NO_WAKE;
        return;
    }

    rep->error = render_state();
    t1 = kosmos_ticks();

    {
        static struct sysinfo machine;     /* large: not on the stack */
        unsigned long per_us;

        per_us = kosmos_sysinfo(&machine) == 0 && machine.counter_hz >= 1000000u
               ? machine.counter_hz / 1000000u : 1u;
        rep->engine.elapsed_us = (uint32_t)((t1 - t0) / per_us);
    }

    (void)forcewake(true);
    rep->engine.value = RCS.ready ? fence_reached(&RCS) : 0;
    rep->engine.head = eng(&RCS, IGPU_RING_HEAD);
    rep->engine.tail = eng(&RCS, IGPU_RING_TAIL);
    rep->engine.mode = eng(&RCS, IGPU_RING_MODE);
    rep->engine.esr = eng(&RCS, IGPU_RING_ESR);

    for (i = 0; i < 100 && !engine_idle(&RCS); i++) {
        kosmos_sleep(1);
    }

    say_begin(&line);
    say_text(&line, "intelgpu: the render engine's starting state ");
    say_text(&line, rep->error != GPU_OK ? "did not finish" : before ? "was run already" : "ran");
    say_text(&line, "; head ");
    say_hex(&line, rep->engine.head, 8);
    say_text(&line, " tail ");
    say_hex(&line, rep->engine.tail, 8);
    say_text(&line, " esr ");
    say_hex(&line, rep->engine.esr, 8);
    say_send(console, &line);
}

/*
 * **A texture drawn by the 3D pipeline** (`m700-2d.md` 7c): the batch
 * `tools/render_batch.c` wrote from intel-gpu-tools' render copy
 * (`intelgpu_render.h`), its relocations made - the batch's own address,
 * the destination's and the source's added where it says - and run from
 * the ring after the starting state. The source is a pattern every pixel of
 * which differs from its neighbours, alpha included; the destination is
 * held to it pixel for pixel, and the page after it to zero.
 */
#define RT_PAGES  ((INTELGPU_RENDER_PITCH * INTELGPU_RENDER_H + 4095u) / 4096u)

static volatile uint32_t *rt_src, *rt_dst;
static uint32_t rt_src_gtt, rt_dst_gtt;

static uint32_t rt_pattern(uint32_t x, uint32_t y)
{
    return ((x * 4u) & 0xffu) | (((y * 4u) & 0xffu) << 8) | (((x ^ y) * 9u & 0xffu) << 16)
           | ((0x40u + x + y) & 0xffu) << 24;
}

/* The ground a blend is drawn over: opaque, as a window's back buffer is,
 * and differing from the source in every channel. */
static uint32_t rt_ground(uint32_t x, uint32_t y)
{
    return 0xff000000u | ((255u - x * 3u) & 0xffu) << 16 | ((x * y) & 0xffu) << 8 | ((y * 5u) & 0xffu);
}

/* The source a blend lays over it: every alpha from 0 to 255 across the
 * rows, 0 and 255 among them. */
static uint32_t rt_blend_source(uint32_t x, uint32_t y)
{
    uint32_t a = (y * INTELGPU_BLEND_W + x) & 0xffu;

    return (rt_pattern(x, y) & 0x00ffffffu) | a << 24;
}

/* The largest difference in any one channel between two pixels. */
static uint32_t rt_apart(uint32_t a, uint32_t b)
{
    uint32_t most = 0, k;

    for (k = 0; k < 32; k += 8) {
        int d = (int)((a >> k) & 0xffu) - (int)((b >> k) & 0xffu);

        if (d < 0) {
            d = -d;
        }

        if ((uint32_t)d > most) {
            most = (uint32_t)d;
        }
    }

    return most;
}

/*
 * **A picture on the GPU, for drawing**: where it is in the GTT, its size
 * and its pitch in bytes.
 */
struct gpu_picture {
    uint32_t gtt;
    uint32_t width, height, pitch;
    bool     scanout;                   /* a screen the display reads */
};

/*
 * **How a surface is cached, by what it is** (7d): RENDER_SURFACE_STATE's
 * MOCS, bits 24-30 of dword 1, an index shifted left one. A screen, index
 * 0 - uncached, as i915 keeps every scanout: the display reads memory and
 * not the LLC, and pixels the render engine left there were not shown -
 * nor seen by the window server, whose mapping of the screen is uncached,
 * which read 2% of a frame's pixels as the frame before. Anything else,
 * index 2 - write-back in the LLC, which the processor's caches are kept in
 * step with, so the sampler reads what the compositor wrote a moment ago
 * and the compositor what the render engine drew.
 */
static uint32_t surface_mocs(const struct gpu_picture *p)
{
    return (p->scanout ? 0u : 2u) << 1;
}

static void put_float(volatile uint32_t *at, float f)
{
    union { float f; uint32_t u; } v;

    v.f = f;
    *at = v.u;
}

/*
 * **Rectangles of one picture drawn onto another by the 3D pipeline**
 * (`m700-2d.md` 7c, 7d): each `w` by `h` from `sx`, `sy` in `src` to `x`,
 * `y` in `dst` - `struct gpu_blit`'s rectangles - copied or, `blend`, laid
 * over it by its alpha as the compositor lays a window. The batch
 * `tools/render_batch.c` wrote, with what depends on the pictures filled
 * in: each surface's address, size and pitch, the drawing rectangle (the
 * whole target), and three corners a rectangle in the vertex buffer - the
 * target's in pixels, the source's in coordinates from 0 to 1, a pixel's
 * centre falling on its texel so the nearest sampler takes it whole - with
 * the buffer's size and the vertex count to match.
 *
 * **Not waited for**: the batch goes into a page of its own from a pool,
 * so several can be in the ring at once - a frame's windows and the copy to
 * the screen after them - and the answer is its fence. A page whose last
 * batch has not finished is not written over: GPU_ERR_BUSY, and the caller
 * tries again.
 */
#define DRAW_PAGES   32u
#define DRAW_RECTS   ((4096u - INTELGPU_RENDER_VERTICES) / (3u * INTELGPU_RENDER_VERTEX))

static volatile uint32_t *draw_page[DRAW_PAGES];
static uint32_t draw_page_gtt[DRAW_PAGES], draw_page_fence[DRAW_PAGES];
static unsigned draw_next;
static uint32_t draws;                  /* batches given the render engine, since boot */

struct draw_rect { int32_t x, y, w, h, sx, sy; };

static uint32_t draw_batch(const struct gpu_picture *dst, const struct gpu_picture *src,
                           const struct draw_rect *r, uint32_t n, bool blend, uint32_t *fence)
{
    const uint32_t *batch = blend ? intelgpu_blend : intelgpu_render;
    const uint32_t words = blend ? sizeof(intelgpu_blend) / 4u : sizeof(intelgpu_render) / 4u;
    volatile uint32_t *b, *ss, *v;
    uint32_t error, i, page_gtt;
    unsigned at;

    _Static_assert(INTELGPU_RENDER_SS_DST == INTELGPU_BLEND_SS_DST
                   && INTELGPU_RENDER_SS_SRC == INTELGPU_BLEND_SS_SRC
                   && INTELGPU_RENDER_VERTICES == INTELGPU_BLEND_VERTICES
                   && INTELGPU_RENDER_DRAWRECT == INTELGPU_BLEND_DRAWRECT
                   && INTELGPU_RENDER_VBSIZE == INTELGPU_BLEND_VBSIZE
                   && INTELGPU_RENDER_COUNT == INTELGPU_BLEND_COUNT
                   && sizeof(intelgpu_render_relocs) == sizeof(intelgpu_blend_relocs),
                   "the copy's batch and the blend's differ only in the blending");

    if (n == 0 || n > DRAW_RECTS || dst->width > 16384u || dst->height > 16384u
        || src->width > 16384u || src->height > 16384u || dst->width == 0 || src->width == 0) {
        return GPU_ERR_BAD_OP;
    }

    for (i = 0; i < n; i++) {
        if (r[i].w <= 0 || r[i].h <= 0 || r[i].x < 0 || r[i].y < 0 || r[i].sx < 0 || r[i].sy < 0
            || (uint32_t)(r[i].sx + r[i].w) > src->width || (uint32_t)(r[i].sy + r[i].h) > src->height
            || (uint32_t)(r[i].x + r[i].w) > dst->width || (uint32_t)(r[i].y + r[i].h) > dst->height) {
            return GPU_ERR_BAD_OP;
        }
    }

    if ((error = render_state()) != GPU_OK) {
        return error;
    }

    at = draw_next;

    if (draw_page[at] == NULL && !gtt_pages(1, &draw_page[at], &draw_page_gtt[at])) {
        return GPU_ERR_NO_MEMORY;
    }

    /* The page's last batch done, and room in the ring for this one. */
    if ((int32_t)(fence_reached(&RCS) - draw_page_fence[at]) < 0
        || RCS.in_flight + 48u > RING_ROOM) {
        (void)engine_idle(&RCS);
        return GPU_ERR_BUSY;
    }

    (void)engine_idle(&RCS);
    b = draw_page[at];
    page_gtt = draw_page_gtt[at];

    for (i = 0; i < words; i++) {
        b[i] = batch[i];
    }

    for (i = 0; i < sizeof(intelgpu_render_relocs) / sizeof(intelgpu_render_relocs[0]); i++) {
        uint32_t w = (blend ? intelgpu_blend_relocs[i].at : intelgpu_render_relocs[i].at) / 4u;
        uint32_t kind = blend ? intelgpu_blend_relocs[i].kind : intelgpu_render_relocs[i].kind;

        b[w] += kind == 1u ? page_gtt : kind == 2u ? dst->gtt : src->gtt;
    }

    /* Each surface's size and pitch: RENDER_SURFACE_STATE's dwords 2 and 3. */
    ss = b + INTELGPU_RENDER_SS_DST / 4u;
    ss[1] = (ss[1] & ~(0x7fu << 24)) | surface_mocs(dst) << 24;
    ss[2] = (dst->width - 1u) | (dst->height - 1u) << 16;
    ss[3] = dst->pitch - 1u;
    ss = b + INTELGPU_RENDER_SS_SRC / 4u;
    ss[1] = (ss[1] & ~(0x7fu << 24)) | surface_mocs(src) << 24;
    ss[2] = (src->width - 1u) | (src->height - 1u) << 16;
    ss[3] = src->pitch - 1u;

    /* The drawing rectangle, the whole target: its maximum in dword 2. */
    b[INTELGPU_RENDER_DRAWRECT / 4u + 2u] = (dst->width - 1u) | (dst->height - 1u) << 16;

    /* Each rectangle a rectangle list's three corners: bottom right, bottom
     * left, top left. */
    v = b + INTELGPU_RENDER_VERTICES / 4u;

    for (i = 0; i < n; i++, v += 9) {
        uint32_t x0 = (uint32_t)r[i].x, y0 = (uint32_t)r[i].y;
        uint32_t x1 = x0 + (uint32_t)r[i].w, y1 = y0 + (uint32_t)r[i].h;
        float u0 = (float)r[i].sx / (float)src->width, u1 = (float)(r[i].sx + r[i].w) / (float)src->width;
        float v0 = (float)r[i].sy / (float)src->height, v1 = (float)(r[i].sy + r[i].h) / (float)src->height;

        v[0] = x1 | y1 << 16;
        put_float(&v[1], u1);
        put_float(&v[2], v1);
        v[3] = x0 | y1 << 16;
        put_float(&v[4], u0);
        put_float(&v[5], v1);
        v[6] = x0 | y0 << 16;
        put_float(&v[7], u0);
        put_float(&v[8], v0);
    }

    b[INTELGPU_RENDER_VBSIZE / 4u] = 3u * n * INTELGPU_RENDER_VERTEX;
    b[INTELGPU_RENDER_COUNT / 4u] = 3u * n;

    {
        /*
         * **The caches invalidated as i915 does it** (`gen8_emit_flush_rcs`):
         * an empty PIPE_CONTROL first, which generation 9 wants before the
         * vertex fetcher's invalidation; then the instruction, texture,
         * vertex, constant and state caches and the TLB invalidated, with a
         * post-sync write - without which the invalidation is not sure to
         * happen. The texture cache kept the back buffer's lines from the
         * frame before, and the copy to the screen read them where the
         * compositor had drawn since (7d).
         */
        const uint32_t null_pc[6] = { IGPU_PIPE_CONTROL, 0, 0, 0, 0, 0 };
        const uint32_t before[6] = { IGPU_PIPE_CONTROL,
                                     IGPU_PC_CS_STALL | IGPU_PC_TLB_INVALIDATE | IGPU_PC_INVALIDATE
                                         | IGPU_PC_VF_INVALIDATE | IGPU_PC_QW_WRITE | IGPU_PC_GLOBAL_GTT,
                                     RCS.status_gtt + SCRATCH_AT * 4u, 0, 0, 0 };
        const uint32_t start[3] = { IGPU_MI_BB_START, page_gtt, 0 };
        const uint32_t after[6] = { IGPU_PIPE_CONTROL, IGPU_PC_CS_STALL | IGPU_PC_RT_FLUSH | IGPU_PC_DC_FLUSH,
                                    0, 0, 0, 0 };

#if defined(__x86_64__)
        __asm__ volatile("sfence" ::: "memory");
#endif
        emit(&RCS, null_pc, 6);
        emit(&RCS, before, 6);
        emit(&RCS, start, 3);
        emit(&RCS, after, 6);
    }

    *fence = submit(&RCS);
    draw_page_fence[at] = *fence;
    draw_next = (draw_next + 1u) % DRAW_PAGES;
    draws++;
    return GPU_OK;
}

/* One rectangle drawn and waited for: the tests'. */
static uint32_t draw(const struct gpu_picture *dst, const struct gpu_picture *src,
                     uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
                     uint32_t dx, uint32_t dy, bool blend, unsigned long *took)
{
    const struct draw_rect r = { (int32_t)dx, (int32_t)dy, (int32_t)w, (int32_t)h, (int32_t)sx, (int32_t)sy };
    uint32_t error, fence = 0, i;
    unsigned long t0 = kosmos_ticks();

    if ((error = draw_batch(dst, src, &r, 1, blend, &fence)) != GPU_OK) {
        return error;
    }

    for (i = 0; i < 20000 && (int32_t)(fence_reached(&RCS) - fence) < 0; i++) {
        __asm__ volatile("" ::: "memory");
    }

    for (i = 0; i < 200 && (int32_t)(fence_reached(&RCS) - fence) < 0; i++) {
        kosmos_sleep(1);
    }

    if (took != NULL) {
        static struct sysinfo machine;     /* large: not on the stack */
        unsigned long per_us;

        per_us = kosmos_sysinfo(&machine) == 0 && machine.counter_hz >= 1000000u
               ? machine.counter_hz / 1000000u : 1u;
        *took = (kosmos_ticks() - t0) / per_us;
    }

    return (int32_t)(fence_reached(&RCS) - fence) >= 0 ? GPU_OK : GPU_ERR_NO_ANSWER;
}

/*
 * **A request's rectangles drawn** (7d, `GPU_OP_DRAW`): from one buffer
 * into another of the GPU's table, as `blit` copies them, in batches of
 * DRAW_RECTS; the fence of the last in `rep->engine.value`.
 */
static uint32_t draw_request(const struct gpu_blit *q, bool blend, struct gpu_reply *rep)
{
    struct gpu_picture d, s;
    struct draw_rect r[DRAW_RECTS];
    uint32_t done = 0, error = GPU_OK, fence = 0;

    if (!G.found) {
        return GPU_ERR_NO_DEVICE;
    }

    if (q->src >= BUFFERS_MAX || q->dst >= BUFFERS_MAX || !B[q->src].used || !B[q->dst].used
        || q->n == 0 || q->n > GPU_BLIT_RECTS) {
        return GPU_ERR_NO_BUFFER;
    }

    d = (struct gpu_picture){ B[q->dst].gtt, B[q->dst].width, B[q->dst].height, B[q->dst].pitch,
                              B[q->dst].kind == GPU_BUFFER_SCANOUT };
    s = (struct gpu_picture){ B[q->src].gtt, B[q->src].width, B[q->src].height, B[q->src].pitch,
                              B[q->src].kind == GPU_BUFFER_SCANOUT };

    if (!forcewake(true)) {
        return GPU_ERR_NO_WAKE;
    }

    while (done < q->n && error == GPU_OK) {
        uint32_t k = 0;

        while (done + k < q->n && k < DRAW_RECTS) {
            r[k] = (struct draw_rect){ q->rects[done + k].x, q->rects[done + k].y, q->rects[done + k].w,
                                       q->rects[done + k].h, q->rects[done + k].sx, q->rects[done + k].sy };
            k++;
        }

        error = draw_batch(&d, &s, r, k, blend, &fence);
        done += k;
    }

    rep->engine.value = fence;
    return error;
}

/*
 * **The pictures held to the compositor**, after a draw: every pixel of
 * `dst` inside the rectangle against `want` - the source's, or `over` it
 * on the ground, the compositor's own arithmetic from `kits/gfx/surface.h`
 * - and every pixel outside against the ground, untouched. A blended pixel
 * passes within one in every channel with its alpha 255 (the compositor
 * rounds each of its two products, the GPU's blender once); a copied one,
 * and one outside, exactly. Answers the pixels that pass, `exact` those
 * equal, `worst` the largest difference, `outside` those outside that
 * changed.
 */
struct held { uint32_t right, exact, worst, outside, pixels; };

static struct held hold(const volatile uint32_t *dst, const struct gpu_picture *d,
                        const volatile uint32_t *src, const struct gpu_picture *s,
                        uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, uint32_t dx, uint32_t dy,
                        bool blend, uint32_t (*ground)(uint32_t x, uint32_t y))
{
    struct held r = { 0, 0, 0, 0, d->width * d->height };
    uint32_t x, y;

    for (y = 0; y < d->height; y++) {
        for (x = 0; x < d->width; x++) {
            uint32_t got = dst[y * (d->pitch / 4u) + x];
            uint32_t was = ground(x, y);
            bool in = x >= dx && x < dx + w && y >= dy && y < dy + h;
            uint32_t want = was, apart;

            if (in) {
                uint32_t sp = src[(sy + y - dy) * (s->pitch / 4u) + sx + x - dx];

                want = blend ? over(sp, was, 255u) : sp;
            }

            apart = rt_apart(got, want);
            r.exact += apart == 0;
            r.worst = apart > r.worst ? apart : r.worst;

            if (in && blend) {
                r.right += apart <= 1u && (got >> 24) == 255u;
            } else {
                r.right += apart == 0;
                r.outside += !in && apart != 0;
            }
        }
    }

    return r;
}

static uint32_t rt_zero(uint32_t x, uint32_t y)
{
    (void)x; (void)y;
    return 0;
}

/*
 * **The copy, or the blend, of a 64 by 64 texture** (7c): `gpu render
 * [blend]`. The source a pattern every pixel of which differs from its
 * neighbours - for the blend, with every alpha from 0 to 255 - and the
 * ground nothing for the copy, an opaque pattern for the blend. The reply's
 * `value` is the pixels that pass, `mode` the exact ones, `ctl` the bytes
 * written past the destination.
 */
static void render_test(long console, bool blend, struct gpu_reply *rep)
{
    struct say_line line;
    struct gpu_picture d = { 0, INTELGPU_RENDER_W, INTELGPU_RENDER_H, INTELGPU_RENDER_PITCH, false };
    struct gpu_picture s = d;
    struct held r = { 0, 0, 0, 0, 0 };
    uint32_t error = GPU_OK, past = 0, i;
    unsigned long took = 0;

    if (!G.found) {
        rep->error = GPU_ERR_NO_DEVICE;
        return;
    }

    if (!forcewake(true)) {
        rep->error = GPU_ERR_NO_WAKE;
        return;
    }

    if (rt_src == NULL
        && (!gtt_pages(RT_PAGES, &rt_src, &rt_src_gtt) || !gtt_pages(RT_PAGES + 1u, &rt_dst, &rt_dst_gtt))) {
        error = GPU_ERR_NO_MEMORY;
    }

    if (error == GPU_OK) {
        d.gtt = rt_dst_gtt;
        s.gtt = rt_src_gtt;

        for (i = 0; i < (RT_PAGES + 1u) * 1024u; i++) {
            rt_dst[i] = 0;
        }

        for (i = 0; i < d.width * d.height; i++) {
            uint32_t x = i % d.width, y = i / d.width;

            rt_src[i] = blend ? rt_blend_source(x, y) : rt_pattern(x, y);
            rt_dst[i] = blend ? rt_ground(x, y) : 0;
        }

        error = draw(&d, &s, 0, 0, d.width, d.height, 0, 0, blend, &took);
        r = hold(rt_dst, &d, rt_src, &s, 0, 0, d.width, d.height, 0, 0, blend,
                 blend ? rt_ground : rt_zero);

        for (i = d.width * d.height; i < d.width * d.height + 1024u; i++) {
            past += rt_dst[i] != 0;
        }

        if (error == GPU_OK && (r.right != r.pixels || past != 0)) {
            error = GPU_ERR_UNCHANGED;
        }
    }

    (void)forcewake(true);
    rep->engine.value = r.right;
    rep->engine.mode = r.exact;
    rep->engine.ctl = past;
    rep->engine.elapsed_us = (uint32_t)took;
    rep->engine.head = eng(&RCS, IGPU_RING_HEAD);
    rep->engine.tail = eng(&RCS, IGPU_RING_TAIL);
    rep->engine.esr = eng(&RCS, IGPU_RING_ESR);
    rep->error = error;

    for (i = 0; i < 100 && !engine_idle(&RCS); i++) {
        kosmos_sleep(1);
    }

    say_begin(&line);
    say_text(&line, blend ? "intelgpu: the 3D pipeline blended " : "intelgpu: the 3D pipeline drew ");
    say_dec(&line, r.right);
    say_text(&line, blend ? " of 4096 pixels within one of the compositor's, " : " of 4096 pixels as the texture has them, ");
    say_dec(&line, r.exact);
    say_text(&line, " exactly, at most ");
    say_dec(&line, r.worst);
    say_text(&line, " apart, ");
    say_dec(&line, past);
    say_text(&line, " bytes past; head ");
    say_hex(&line, rep->engine.head, 8);
    say_text(&line, " tail ");
    say_hex(&line, rep->engine.tail, 8);
    say_text(&line, " esr ");
    say_hex(&line, rep->engine.esr, 8);
    say_send(console, &line);
}

/*
 * **A window at a window's proportions** (7c): `gpu draw`. A 301 by 203
 * picture whose pitch is wider than its width, a 280 by 190 part of it from
 * (7, 5), blended onto an 800 by 600 target - its pitch wider again - at
 * (123, 77): every size odd, every pitch not the width, the source and the
 * target places not the origin. Every pixel of the target held to the
 * compositor's arithmetic inside the rectangle and to the ground outside.
 * The reply's `value` is the pixels that pass of the 480,000, `mode` the
 * exact ones, `ctl` the ones outside that changed.
 */
#define DT_SRC_W 301u
#define DT_SRC_H 203u
#define DT_SRC_PITCH 1216u              /* 301 * 4 is 1204: 64 bytes a step */
#define DT_DST_W 800u
#define DT_DST_H 600u
#define DT_DST_PITCH 3328u              /* 800 * 4 is 3200 */

static volatile uint32_t *dt_src, *dt_dst;
static uint32_t dt_src_gtt, dt_dst_gtt;

static uint32_t dt_ground(uint32_t x, uint32_t y)
{
    return 0xff000000u | ((x * 7u) & 0xffu) << 16 | ((y * 3u) & 0xffu) << 8 | ((x + y) & 0xffu);
}

/*
 * **A screen-sized copy, alone** (7d): a 1720 by 1440 pattern, in the
 * driver's own write-combining memory, copied whole onto a target filled
 * with a marker - so no processor's cache can be in it - and every pixel
 * counted: written right, written wrong, or never written (the marker
 * still there). `gpu draw big`.
 */
#define BIG_W     1720u
#define BIG_H     1440u
#define BIG_MARK  0x5a17c0deu

static volatile uint32_t *big_src, *big_dst;
static uint32_t big_src_gtt, big_dst_gtt;

static void big_test(long console, struct gpu_reply *rep)
{
    struct say_line line;
    struct gpu_picture d = { 0, BIG_W, BIG_H, BIG_W * 4u, false }, s = d;
    uint32_t error = GPU_OK, right = 0, wrong = 0, unwritten = 0, first = ~0u, i;
    unsigned long took = 0;

    if (!G.found) {
        rep->error = GPU_ERR_NO_DEVICE;
        return;
    }

    if (!forcewake(true)) {
        rep->error = GPU_ERR_NO_WAKE;
        return;
    }

    if (big_src == NULL
        && (!gtt_pages((BIG_W * BIG_H * 4u + 4095u) / 4096u, &big_src, &big_src_gtt)
            || !gtt_pages((BIG_W * BIG_H * 4u + 4095u) / 4096u, &big_dst, &big_dst_gtt))) {
        error = GPU_ERR_NO_MEMORY;
    }

    if (error == GPU_OK) {
        d.gtt = big_dst_gtt;
        s.gtt = big_src_gtt;

        for (i = 0; i < BIG_W * BIG_H; i++) {
            big_src[i] = rt_pattern(i % BIG_W, i / BIG_W) | 0xff000000u;
            big_dst[i] = BIG_MARK;
        }

#if defined(__x86_64__)
        __asm__ volatile("sfence" ::: "memory");
#endif
        error = draw(&d, &s, 0, 0, BIG_W, BIG_H, 0, 0, false, &took);

        for (i = 0; i < BIG_W * BIG_H; i++) {
            if (big_dst[i] == big_src[i]) {
                right++;
            } else {
                if (first == ~0u) {
                    first = i;
                }

                unwritten += big_dst[i] == BIG_MARK;
                wrong += big_dst[i] != BIG_MARK;
            }
        }

        if (error == GPU_OK && right != BIG_W * BIG_H) {
            error = GPU_ERR_UNCHANGED;
        }
    }

    rep->engine.value = right;
    rep->engine.mode = unwritten;
    rep->engine.ctl = wrong;
    rep->engine.elapsed_us = (uint32_t)took;
    rep->engine.esr = eng(&RCS, IGPU_RING_ESR);
    rep->error = error;

    for (i = 0; i < 100 && !engine_idle(&RCS); i++) {
        kosmos_sleep(1);
    }

    say_begin(&line);
    say_text(&line, "intelgpu: a 1720x1440 copy: ");
    say_dec(&line, right);
    say_text(&line, " right, ");
    say_dec(&line, unwritten);
    say_text(&line, " never written, ");
    say_dec(&line, wrong);
    say_text(&line, " wrong");

    if (first != ~0u && big_dst != NULL) {
        say_text(&line, "; the first at ");
        say_dec(&line, first % BIG_W);
        say_text(&line, ",");
        say_dec(&line, first / BIG_W);
        say_text(&line, ": ");
        say_hex(&line, big_dst[first], 8);
        say_text(&line, " for ");
        say_hex(&line, big_src[first], 8);
    }

    say_send(console, &line);
}

static void draw_test(long console, struct gpu_reply *rep)
{
    struct say_line line;
    struct gpu_picture d = { 0, DT_DST_W, DT_DST_H, DT_DST_PITCH, false };
    struct gpu_picture s = { 0, DT_SRC_W, DT_SRC_H, DT_SRC_PITCH, false };
    const uint32_t sx = 7, sy = 5, w = 280, h = 190, dx = 123, dy = 77;
    struct held r = { 0, 0, 0, 0, 0 };
    uint32_t error = GPU_OK, x, y, i;
    unsigned long took = 0;

    if (!G.found) {
        rep->error = GPU_ERR_NO_DEVICE;
        return;
    }

    if (!forcewake(true)) {
        rep->error = GPU_ERR_NO_WAKE;
        return;
    }

    if (dt_src == NULL
        && (!gtt_pages((DT_SRC_PITCH * DT_SRC_H + 4095u) / 4096u, &dt_src, &dt_src_gtt)
            || !gtt_pages((DT_DST_PITCH * DT_DST_H + 4095u) / 4096u, &dt_dst, &dt_dst_gtt))) {
        error = GPU_ERR_NO_MEMORY;
    }

    if (error == GPU_OK) {
        d.gtt = dt_dst_gtt;
        s.gtt = dt_src_gtt;

        for (y = 0; y < DT_SRC_H; y++) {
            for (x = 0; x < DT_SRC_W; x++) {
                dt_src[y * (DT_SRC_PITCH / 4u) + x] = (rt_pattern(x, y) & 0x00ffffffu)
                                                      | ((x * 5u + y * 3u) & 0xffu) << 24;
            }
        }

        for (y = 0; y < DT_DST_H; y++) {
            for (x = 0; x < DT_DST_W; x++) {
                dt_dst[y * (DT_DST_PITCH / 4u) + x] = dt_ground(x, y);
            }
        }

        error = draw(&d, &s, sx, sy, w, h, dx, dy, true, &took);
        r = hold(dt_dst, &d, dt_src, &s, sx, sy, w, h, dx, dy, true, dt_ground);

        if (error == GPU_OK && r.right != r.pixels) {
            error = GPU_ERR_UNCHANGED;
        }
    }

    (void)forcewake(true);
    rep->engine.value = r.right;
    rep->engine.mode = r.exact;
    rep->engine.ctl = r.outside;
    rep->engine.elapsed_us = (uint32_t)took;
    rep->engine.head = eng(&RCS, IGPU_RING_HEAD);
    rep->engine.tail = eng(&RCS, IGPU_RING_TAIL);
    rep->engine.esr = eng(&RCS, IGPU_RING_ESR);
    rep->error = error;

    for (i = 0; i < 100 && !engine_idle(&RCS); i++) {
        kosmos_sleep(1);
    }

    say_begin(&line);
    say_text(&line, "intelgpu: 280x190 of a 301x203 picture blended onto 800x600 at 123,77: ");
    say_dec(&line, r.right);
    say_text(&line, " of 480000 pass, ");
    say_dec(&line, r.exact);
    say_text(&line, " exactly, at most ");
    say_dec(&line, r.worst);
    say_text(&line, " apart, ");
    say_dec(&line, r.outside);
    say_text(&line, " outside changed; esr ");
    say_hex(&line, rep->engine.esr, 8);
    say_send(console, &line);
}

/*
 * **A region another process drew in, handed to the GPU** (step 6): each
 * of its pages entered in a GTT slot of its own, from `kosmos_mem_page_phys`,
 * so it need not be one run. The window server's back buffer is the
 * first. The caller keeps drawing in it; the GPU reads it where it is.
 */
static uint32_t buffer_import(long console, long cap, const struct gpu_request *rq,
                              struct gpu_reply *rep)
{
    struct say_line line;
    unsigned width = (unsigned)rq->x, height = (unsigned)rq->y, id;
    long pages;
    unsigned long i, first;

    if (!G.found) {
        return GPU_ERR_NO_DEVICE;
    }

    buffers_init();

    if (cap < 0 || rq->x <= 0 || rq->y <= 0 || width > 16384u || height > 16384u
        || rq->pitch < width * 4u || rq->pitch >= 32768u || (pages = kosmos_mem_size(cap)) <= 0
        || (unsigned long)rq->offset + (unsigned long)rq->pitch * height > (unsigned long)pages * 4096ul) {
        return GPU_ERR_BAD_OP;
    }

    for (id = 1; id < BUFFERS_MAX && B[id].used; id++) {
    }

    if (id == BUFFERS_MAX) {
        return GPU_ERR_NO_MEMORY;
    }

    if (next_slot + (unsigned long)pages > GTT_ENTRIES) {
        return GPU_ERR_GTT_BUSY;
    }

    first = next_slot;

    for (i = 0; i < (unsigned long)pages; i++) {
        long phys = kosmos_mem_page_phys(cap, i);

        if (phys < 0 || !gtt_write(first + i, (uint64_t)phys | 1u, NULL)) {
            return GPU_ERR_NO_MEMORY;
        }
    }

    wreg(GFX_FLSH_CNTL, 1u);
    (void)reg(G.regs, GFX_FLSH_CNTL);

    B[id].used = true;
    B[id].kind = GPU_BUFFER_IMPORTED;
    B[id].holder = holder_of_request();
    B[id].cap = cap;                    /* kept: the pages stay while the GPU can reach them */
    B[id].slot = first;
    B[id].gtt = (uint32_t)(first << 12) + rq->offset;
    B[id].pages = (unsigned long)pages;
    B[id].width = width;
    B[id].height = height;
    B[id].pitch = rq->pitch;
    next_slot = (first + (unsigned long)pages + 255u) & ~255ul;

    rep->buffer.id = id;
    rep->buffer.gtt = B[id].gtt;
    rep->buffer.pitch = rq->pitch;
    rep->buffer.pages = (uint32_t)pages;

    say_begin(&line);
    say_text(&line, "intelgpu: buffer ");
    say_dec(&line, id);
    say_text(&line, " imported, ");
    say_dec(&line, width);
    say_text(&line, "x");
    say_dec(&line, height);
    say_text(&line, " in ");
    say_dec(&line, (unsigned long)pages);
    say_text(&line, " pages, at GTT ");
    say_hex(&line, B[id].gtt, 8);
    say_send(console, &line);

    return GPU_OK;
}

/* An imported buffer let go: its slots emptied, its region given back. */
static uint32_t buffer_forget(const struct gpu_request *rq)
{
    unsigned id = (unsigned)rq->x;
    unsigned long i;

    if (rq->x <= 0 || id >= BUFFERS_MAX || !B[id].used || B[id].kind != GPU_BUFFER_IMPORTED) {
        return GPU_ERR_NO_BUFFER;
    }

    if (B[id].holder != holder_of_request()) {
        return GPU_ERR_NOT_YOURS;
    }

    if (!engine_idle(&BCS) || !engine_idle(&RCS)) {
        return GPU_ERR_BUSY;            /* the blitter, or a draw or a run, may still be reading it */
    }

    for (i = 0; i < B[id].pages; i++) {
        (void)gtt_write(B[id].slot + i, 0, NULL);
    }

    wreg(GFX_FLSH_CNTL, 1u);
    (void)reg(G.regs, GFX_FLSH_CNTL);
    (void)kosmos_cap_drop(B[id].cap);
    memset(&B[id], 0, sizeof B[id]);
    return GPU_OK;
}

/*
 * **Copies on the blitter** (step 6): each rectangle from one buffer to the
 * same place in another, clipped to both, as XY_SRC_COPY_BLT; then the
 * fence. Answered at once with the fence to look for - nothing here waits
 * for the GPU. Refused while the last is still running: the ring is then
 * always free when written, and a caller has one thing in flight.
 */
static uint32_t blit(const struct gpu_blit *b, struct gpu_reply *rep)
{
    const struct buffer *src, *dst;
    uint32_t error, k, given = 0, timed;

    if (!G.found) {
        return GPU_ERR_NO_DEVICE;
    }

    if (b->src >= BUFFERS_MAX || b->dst >= BUFFERS_MAX || !B[b->src].used || !B[b->dst].used
        || b->n > GPU_BLIT_RECTS) {
        return GPU_ERR_NO_BUFFER;
    }

    src = &B[b->src];
    dst = &B[b->dst];

    if ((error = engine_start(&BCS)) != GPU_OK) {
        return error;
    }

    (void)engine_idle(&BCS);

    /* Room for it all - ten dwords a copy and twenty around them - or not
     * now: several may be in flight, never more than the ring holds. */
    if (BCS.in_flight + b->n * 10u + 40u > RING_ROOM
        || (int32_t)(BCS.issued + 1u - BCS.counted) > (int32_t)TS_SLOTS - 1) {
        return GPU_ERR_BUSY;
    }

    /* The engine's clock stored first, and again after the copies have
     * been flushed to memory: the difference is their time (G3), in the
     * slot of the fence they end with. */
    timed = TS_AT + 2u * ((BCS.issued + 1u) % TS_SLOTS);

    {
        uint32_t dw[4] = { IGPU_MI_STORE_REG_MEM, IGPU_BLT_BASE + IGPU_RING_TIMESTAMP,
                           BCS.status_gtt + timed * 4u, 0 };
        emit(&BCS, dw, 4);
    }

    for (k = 0; k < b->n; k++) {
        long x = b->rects[k].x, y = b->rects[k].y;
        long w = b->rects[k].w, h = b->rects[k].h;
        long sx = b->rects[k].sx, sy = b->rects[k].sy;
        long x1, y1;
        uint32_t dw[10];

        /* Held to both buffers: the destination's place and the source's,
         * each clipped, the other moved with it. */
        if (x < 0) { sx -= x; w += x; x = 0; }
        if (y < 0) { sy -= y; h += y; y = 0; }
        if (sx < 0) { x -= sx; w += sx; sx = 0; }
        if (sy < 0) { y -= sy; h += sy; sy = 0; }
        if (x + w > (long)dst->width) w = (long)dst->width - x;
        if (y + h > (long)dst->height) h = (long)dst->height - y;
        if (sx + w > (long)src->width) w = (long)src->width - sx;
        if (sy + h > (long)src->height) h = (long)src->height - sy;

        if (w <= 0 || h <= 0) {
            continue;
        }

        x1 = x + w;
        y1 = y + h;

        dw[0] = IGPU_XY_SRC_COPY_BLT;
        dw[1] = IGPU_BLT_DEPTH_32 | IGPU_BLT_ROP_SRC_COPY | dst->pitch;
        dw[2] = (uint32_t)y << 16 | (uint32_t)x;
        dw[3] = (uint32_t)y1 << 16 | (uint32_t)x1;
        dw[4] = dst->gtt;
        dw[5] = 0;
        dw[6] = (uint32_t)sy << 16 | (uint32_t)sx;
        dw[7] = src->pitch;
        dw[8] = src->gtt;
        dw[9] = 0;
        emit(&BCS, dw, 10);
        given++;
    }

    {
        uint32_t dw[8] = { IGPU_MI_FLUSH_DW, 0, 0, 0,
                           IGPU_MI_STORE_REG_MEM, IGPU_BLT_BASE + IGPU_RING_TIMESTAMP,
                           BCS.status_gtt + (timed + 1u) * 4u, 0 };
        emit(&BCS, dw, 8);
    }

    BCS.blits += given;
    rep->engine.value = submit(&BCS);
    rep->engine.head = given;
    return GPU_OK;
}

static uint32_t buffer_new(long console, const struct gpu_request *rq,
                           struct gpu_reply *rep, long *pass)
{
    struct say_line line;
    unsigned width = (unsigned)rq->x, height = (unsigned)rq->y, pitch, id;
    unsigned long pages, i;
    long region, phys;

    if (!G.found) {
        return GPU_ERR_NO_DEVICE;
    }

    if (G.pipe < 0) {
        return GPU_ERR_NO_PIPE;
    }

    buffers_init();

    if (rq->x <= 0 || rq->y <= 0 || width > 16384u || height > 16384u) {
        return GPU_ERR_BAD_OP;
    }

    for (id = 1; id < BUFFERS_MAX && B[id].used; id++) {
    }

    if (id == BUFFERS_MAX) {
        return GPU_ERR_NO_MEMORY;
    }

    pitch = (width * 4u + 63u) & ~63u;  /* PLANE_STRIDE counts 64 bytes, linear */
    pages = ((unsigned long)pitch * height + 4095u) / 4096u;

    if (next_slot + pages > GTT_ENTRIES) {
        return GPU_ERR_GTT_BUSY;
    }

    /* One run, because `kosmos_mem_phys` names only one; write-combining,
     * because the display does not look in the processor's caches. */
    region = kosmos_mem_create_flags(pages, MEM_CONTIGUOUS | MEM_WRITE_COMBINE);

    if (region < 0 || (phys = kosmos_mem_phys(region)) < 0) {
        if (region >= 0) {
            (void)kosmos_cap_drop(region);
        }

        return GPU_ERR_NO_MEMORY;
    }

    for (i = 0; i < pages; i++) {
        if (!gtt_write(next_slot + i, (uint64_t)(phys + (long)(i * 4096u)) | 1u, NULL)) {
            (void)kosmos_cap_drop(region);
            return GPU_ERR_GTT_BUSY;
        }
    }

    wreg(GFX_FLSH_CNTL, 1u);
    (void)reg(G.regs, GFX_FLSH_CNTL);

    B[id].used = true;
    B[id].kind = GPU_BUFFER_SCANOUT;
    B[id].slot = next_slot;
    flips_seen = true;
    B[id].holder = holder_of_request();
    B[id].cap = region;
    B[id].gtt = (uint32_t)(next_slot << 12);
    B[id].pages = pages;
    B[id].width = width;
    B[id].height = height;
    B[id].pitch = pitch;
    next_slot = (next_slot + pages + 255u) & ~255ul;

    rep->buffer.id = id;
    rep->buffer.gtt = B[id].gtt;
    rep->buffer.pitch = pitch;
    rep->buffer.pages = (uint32_t)pages;
    *pass = region;

    say_begin(&line);
    say_text(&line, "intelgpu: buffer ");
    say_dec(&line, id);
    say_text(&line, ", ");
    say_dec(&line, width);
    say_text(&line, "x");
    say_dec(&line, height);
    say_text(&line, " at GTT ");
    say_hex(&line, B[id].gtt, 8);
    say_text(&line, " = memory ");
    say_hex(&line, (unsigned long)phys, 10);
    say_text(&line, ", write-combining");
    say_send(console, &line);

    return GPU_OK;
}

static void flip_state(struct gpu_reply *rep)
{
    uint32_t step = (uint32_t)G.pipe * IGPU_PIPE_STRIDE;
    unsigned id;

    rep->flip.flips = reg(G.regs, IGPU_PIPE_FLIPCOUNT_A + step);
    rep->flip.live = reg(G.regs, IGPU_PLANE_SURFLIVE_1_A + step) & ~0xFFFu;
    rep->flip.shown = ~0u;

    for (id = 0; id < BUFFERS_MAX; id++) {
        if (B[id].used && B[id].gtt == rep->flip.live) {
            rep->flip.shown = id;
        }
    }
}

/*
 * The plane pointed at a buffer: its stride, then its surface, whose write
 * is what arms the update - the display takes both at the next vertical
 * blank (i915's `skl_program_plane_arm`: `PLANE_SURF` last). Answered now.
 */
static uint32_t flip(const struct gpu_request *rq, struct gpu_reply *rep)
{
    uint32_t step;
    unsigned id = (unsigned)rq->x;

    if (!G.found || G.pipe < 0) {
        return G.found ? GPU_ERR_NO_PIPE : GPU_ERR_NO_DEVICE;
    }

    buffers_init();

    if (rq->x < 0 || id >= BUFFERS_MAX || !B[id].used
        || B[id].width != G.width || B[id].height != G.height) {
        return GPU_ERR_NO_BUFFER;
    }

    step = (uint32_t)G.pipe * IGPU_PIPE_STRIDE;
    flips_seen = flips_seen || id != 0;
    wreg(IGPU_PLANE_STRIDE_1_A + step, B[id].pitch / 64u);
    wreg(IGPU_PLANE_SURF_1_A + step, B[id].gtt);
    flip_state(rep);
    return GPU_OK;
}

/*
 * **What the GPU is doing** (`docs/gpumonitor.md`, G1): its clock now and
 * its range, its time asleep, the pipe's flips and frames and underrun,
 * what the desktop takes from it, and every buffer the driver made with
 * who holds it. Registers read now; nothing sampled on a clock of its own.
 */
static void stats(struct gpu_stats *st)
{
    uint32_t step, cap;
    unsigned id;

    memset(st, 0, sizeof *st);

    if (!G.found) {
        st->error = GPU_ERR_NO_DEVICE;
        return;
    }

    st->device = G.dev.line;      /* the device id, as `hal/pc/devices.c` gives it */
    st->rpstat_raw = reg(G.regs, IGPU_RPSTAT1);
    st->freq_mhz = ((st->rpstat_raw >> 23) & 0x1FFu) * 50u / 3u;
    cap = reg(G.regs, IGPU_RP_STATE_CAP);
    st->freq_max_mhz = (cap & 0xFFu) * 50u;
    st->freq_min_mhz = ((cap >> 16) & 0xFFu) * 50u;
    st->rc6 = reg(G.regs, IGPU_RC6_RESIDENCY);
    (void)engine_idle(&BCS);
    (void)engine_idle(&RCS);
    st->blitter_busy_us = BCS.ts_hz ? (uint32_t)(BCS.busy_ts * 1000000u / BCS.ts_hz) : 0u;
    st->render_busy_us = RCS.ts_hz ? (uint32_t)(RCS.busy_ts * 1000000u / RCS.ts_hz) : 0u;

    if (G.pipe < 0) {
        st->shown = ~0u;
        return;
    }

    buffers_init();
    step = (uint32_t)G.pipe * IGPU_PIPE_STRIDE;
    st->width = G.width;
    st->height = G.height;
    st->flips = reg(G.regs, IGPU_PIPE_FLIPCOUNT_A + step);
    st->frames = reg(G.regs, IGPU_PIPE_FRMCOUNT_A + step);
    st->underrun = (reg(G.regs, DE_PIPE_IIR_A + 0x10u * (uint32_t)G.pipe) & PIPE_FIFO_UNDERRUN) ? 1u : 0u;
    st->desktop = (C.shown ? GPU_DESKTOP_CURSOR : 0u) | (flips_seen ? GPU_DESKTOP_FLIPS : 0u);

    {
        struct gpu_reply fl;

        flip_state(&fl);
        st->shown = fl.flip.shown;
    }

    for (id = 0; id < BUFFERS_MAX && st->nbuffers < GPU_STATS_BUFFERS; id++) {
        if (B[id].used) {
            unsigned n = st->nbuffers++;

            st->buffers[n].id = id;
            st->buffers[n].holder = B[id].holder;
            st->buffers[n].width = B[id].width;
            st->buffers[n].height = B[id].height;
            st->buffers[n].pages = (uint32_t)B[id].pages;
            st->buffers[n].kind = B[id].kind;
        }
    }

    if (C.ready && st->nbuffers < GPU_STATS_BUFFERS) {
        unsigned n = st->nbuffers++;

        st->buffers[n].id = ~0u;
        st->buffers[n].holder = C.holder;
        st->buffers[n].width = 64;
        st->buffers[n].height = 64;
        st->buffers[n].pages = CURSOR_PAGES;
        st->buffers[n].kind = GPU_BUFFER_CURSOR;
    }
}

void intelgpu_server(long console, long endpoint)
{
    mocs_console = console;
    look(console);

    for (;;) {
        static struct message msg, out;
        uint64_t sender = 0;
        struct gpu_request rq;
        struct gpu_reply rep;
        long cap, pass = -1;

        if (kosmos_receive(endpoint, &msg, &sender, 0, 0) != 0) {
            continue;
        }

        memset(&rep, 0, sizeof rep);
        memset(&rq, 0, sizeof rq);
        cap = msg.cap_plus_one != 0 ? (long)msg.cap_plus_one - 1 : -1;

        if (msg.length < 8) {
            rep.error = GPU_ERR_BAD_OP;
        } else {
            memcpy(&rq, msg.data, msg.length < sizeof rq ? msg.length : sizeof rq);

            switch (rq.op) {
            case GPU_OP_LOOK:
                look(console);
                rep.error = G.found ? GPU_OK : GPU_ERR_NO_DEVICE;
                break;
            case GPU_OP_CURSOR_TEST:
                cursor_test(console, rq.seconds < 1 ? 1 : rq.seconds > 30 ? 30 : rq.seconds, &rep);
                break;
            case GPU_OP_CURSOR_SET:
                rep.error = cursor_set(console, cap, &rq);
                break;
            case GPU_OP_CURSOR_MOVE:
                if (C.ready && C.shown) {
                    cursor_place(rq.x, rq.y);
                } else {
                    rep.error = GPU_ERR_NO_PIPE;
                }
                break;
            case GPU_OP_CURSOR_HIDE:
                cursor_hide();
                break;
            case GPU_OP_BUFFER_NEW:
                rep.error = buffer_new(console, &rq, &rep, &pass);
                break;
            case GPU_OP_FLIP:
                rep.error = flip(&rq, &rep);
                break;
            case GPU_OP_STATS:
                break;              /* answered below, in its own shape */
            case GPU_OP_ENGINE_TEST:
                engine_test(console, rq.x == 1 ? &RCS : &BCS, &rep);
                break;
            case GPU_OP_COMPUTE_TEST:
                compute_test(console, (uint32_t)rq.x, &rep);
                break;
            case GPU_OP_RENDER_STATE:
                render_state_test(console, &rep);
                break;
            case GPU_OP_RENDER_TEST:
                render_test(console, rq.x == 1, &rep);
                break;
            case GPU_OP_DRAW_TEST:
                if (rq.x == 1) {
                    big_test(console, &rep);
                } else {
                    draw_test(console, &rep);
                }
                break;
            case GPU_OP_BUFFER_IMPORT:
                rep.error = buffer_import(console, cap, &rq, &rep);
                if (rep.error == GPU_OK) {
                    cap = -1;           /* kept, as the buffer's */
                }
                break;
            case GPU_OP_BUFFER_FORGET:
                rep.error = buffer_forget(&rq);
                break;
            case GPU_OP_BLIT: {
                static struct gpu_blit b;

                memset(&b, 0, sizeof b);
                memcpy(&b, msg.data, msg.length < sizeof b ? msg.length : sizeof b);
                rep.error = msg.length >= 16 && msg.length >= 16 + b.n * 24u
                          ? blit(&b, &rep) : GPU_ERR_BAD_OP;
                break;
            }
            case GPU_OP_FENCE: {
                /* The blitter's, or with `x` 1 the render engine's (7d). */
                struct engine *e = rq.x == 1 ? &RCS : &BCS;

                rep.engine.value = e->ready ? fence_reached(e) : 0;
                rep.engine.tail = e->issued;
                (void)engine_idle(e);
                break;
            }
            case GPU_OP_DRAW:
            case GPU_OP_DRAW_BLEND: {
                static struct gpu_blit q;

                memset(&q, 0, sizeof q);
                memcpy(&q, msg.data, msg.length < sizeof q ? msg.length : sizeof q);
                rep.error = msg.length >= 16 && msg.length >= 16 + q.n * 24u
                          ? draw_request(&q, rq.op == GPU_OP_DRAW_BLEND, &rep) : GPU_ERR_BAD_OP;
                break;
            }
            case GPU_OP_KERNEL:
                rep.error = kernel_new(console, cap, &rq, &rep);
                break;
            case GPU_OP_KERNEL_FORGET:
                rep.error = kernel_forget(&rq);
                break;
            case GPU_OP_RUN_TIME:
                rep.error = run_time(&rq, &rep);
                break;
            case GPU_OP_RUN: {
                static struct gpu_run q;

                memset(&q, 0, sizeof q);
                memcpy(&q, msg.data, msg.length < sizeof q ? msg.length : sizeof q);
                rep.error = msg.length == sizeof q ? run(&q, &rep) : GPU_ERR_BAD_OP;
                break;
            }
            case GPU_OP_FLIPPED:
                if (G.found && G.pipe >= 0) {
                    buffers_init();
                    flip_state(&rep);
                } else {
                    rep.error = G.found ? GPU_ERR_NO_PIPE : GPU_ERR_NO_DEVICE;
                }
                break;
            default:
                rep.error = GPU_ERR_BAD_OP;
                break;
            }
        }

        /* A region that came with the message is the driver's own copy of
         * the capability: let go once the picture is copied. */
        if (cap >= 0) {
            (void)kosmos_cap_drop(cap);
        }

        memset(&out, 0, sizeof out);

        if (msg.length >= 8 && rq.op == GPU_OP_STATS) {
            static struct gpu_stats st;

            stats(&st);
            out.tag = msg.tag;
            out.length = (uint32_t)sizeof st;
            memcpy(out.data, &st, sizeof st);
            (void)kosmos_reply(sender, &out);
            continue;
        }

        out.tag = msg.tag;
        out.cap_plus_one = pass >= 0 ? (uint32_t)pass + 1u : 0u;  /* a copy: the driver keeps its own */
        out.length = (uint32_t)sizeof rep;
        memcpy(out.data, &rep, sizeof rep);
        (void)kosmos_reply(sender, &out);
    }
}
