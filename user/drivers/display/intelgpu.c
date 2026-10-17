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

/* One GTT entry, written: the high word first and the present bit last. */
static bool gtt_write(unsigned long index, uint64_t value, uint64_t *was)
{
    unsigned long at = (unsigned long)G.dev.base + IGPU_GTT_AT + index * 8u;
    long page = kosmos_dev_map(at & ~0xFFFul, 1);
    uintptr_t p;

    if (page < 0) {
        return false;
    }

    p = (uintptr_t)page + (at & 0xFFFu);

    if (was != NULL) {
        *was = (uint64_t)mmio_read32(p) | (uint64_t)mmio_read32(p + 4) << 32;
    }

    mmio_write32(p + 4, (uint32_t)(value >> 32));
    mmio_write32(p, (uint32_t)value);
    return true;
}

static void cursor_test(long console, unsigned seconds, struct gpu_reply *rep)
{
    struct say_line line;
    uint32_t step, buf_start, cur_x, cur_y;
    unsigned long index;
    uint64_t was[CURSOR_PAGES];
    long region, mapped, phys;
    unsigned i, plane_start, plane_end;

    if (!G.found) {
        rep->error = GPU_ERR_NO_DEVICE;
        return;
    }

    if (G.pipe < 0) {
        rep->error = GPU_ERR_NO_PIPE;
        return;
    }

    step = (uint32_t)G.pipe * IGPU_PIPE_STRIDE;

    /* The picture, in pages whose addresses the hardware is told. */
    region = kosmos_mem_create_flags(CURSOR_PAGES, MEM_CONTIGUOUS);

    if (region < 0 || (mapped = kosmos_mem_map(region)) < 0
        || (phys = kosmos_mem_phys(region)) < 0) {
        rep->error = GPU_ERR_NO_MEMORY;
        return;
    }

    test_picture((uint32_t *)mapped);
    flush_lines((const void *)mapped, CURSOR_PAGES * 4096u);

    /* GTT slots past the framebuffer's, a megabyte on: free ones, or none. */
    index = (((unsigned long)(G.surface >> 12) + G.fb_pages + 255u) & ~255ul);

    for (i = 0; i < CURSOR_PAGES; i++) {
        uint64_t entry = (uint64_t)(phys + (long)i * 4096) | 1u;

        if (!gtt_write(index + i, entry, &was[i])) {
            rep->error = GPU_ERR_GTT_BUSY;
            return;
        }
    }

    wreg(GFX_FLSH_CNTL, 1u);
    (void)reg(G.regs, GFX_FLSH_CNTL);

    say_begin(&line);
    say_text(&line, "intelgpu: cursor test: picture at memory ");
    say_hex(&line, (unsigned long)phys, 10);
    say_text(&line, ", GTT ");
    say_hex(&line, index << 12, 8);
    say_text(&line, " (its entries were ");
    say_hex(&line, (unsigned long)was[0], 16);
    say_text(&line, " ...)");
    say_send(console, &line);

    /* The cursor's share of the display buffer: just past plane 1's. */
    igpu_decode_buf(reg(G.regs, IGPU_PLANE_BUF_CFG_1_A + step), &plane_start, &plane_end);
    buf_start = plane_end + 1u;

    rep->underrun_before = reg(G.regs, DE_PIPE_IIR_A + 0x10u * (uint32_t)G.pipe) & PIPE_FIFO_UNDERRUN;

    wreg(IGPU_CUR_BUF_CFG_A + step, ((buf_start + CURSOR_BLOCKS - 1u) << 16) | buf_start);
    wreg(IGPU_CUR_WM_A_0 + step, WM_ENABLE | (1u << 14) | 8u);

    for (i = 1; i < IGPU_WM_LEVELS; i++) {
        wreg(IGPU_CUR_WM_A_0 + step + i * 4u, 0);
    }

    wreg(IGPU_CUR_WM_TRANS_A + step, 0);

    cur_x = (G.width - 64u) / 2u;
    cur_y = (G.height - 64u) / 2u;
    wreg(IGPU_CURPOS_A + step, (cur_y << 16) | cur_x);
    wreg(IGPU_CURCNTR_A + step, CURSOR_ARGB_64);
    wreg(IGPU_CURBASE_A + step, (uint32_t)(index << 12));   /* arms the rest */

    kosmos_sleep(25);                   /* a few frames */

    rep->curcntr = reg(G.regs, IGPU_CURCNTR_A + step);
    rep->curbase = reg(G.regs, IGPU_CURBASE_A + step);
    rep->curpos = reg(G.regs, IGPU_CURPOS_A + step);
    rep->buf_cfg = reg(G.regs, IGPU_CUR_BUF_CFG_A + step);
    rep->wm0 = reg(G.regs, IGPU_CUR_WM_A_0 + step);

    say_begin(&line);
    say_text(&line, "intelgpu: cursor test: on at ");
    say_dec(&line, cur_x);
    say_text(&line, ",");
    say_dec(&line, cur_y);
    say_text(&line, "; read back control ");
    say_hex(&line, rep->curcntr, 8);
    say_text(&line, ", base ");
    say_hex(&line, rep->curbase, 8);
    say_text(&line, ", buffer ");
    say_hex(&line, rep->buf_cfg, 8);
    say_text(&line, ", watermark ");
    say_hex(&line, rep->wm0, 8);
    say_send(console, &line);

    if ((rep->curcntr & 0x27u) != CURSOR_ARGB_64 || rep->curbase != (uint32_t)(index << 12)) {
        rep->error = GPU_ERR_UNCHANGED;
    }

    kosmos_sleep((unsigned long)seconds * 250u);   /* ticks: 250 a second */

    rep->underrun_after = reg(G.regs, DE_PIPE_IIR_A + 0x10u * (uint32_t)G.pipe) & PIPE_FIFO_UNDERRUN;

    /* Off, and everything as the firmware left it. */
    wreg(IGPU_CURCNTR_A + step, 0);
    wreg(IGPU_CURBASE_A + step, 0);
    kosmos_sleep(25);
    wreg(IGPU_CUR_WM_A_0 + step, 0);
    wreg(IGPU_CUR_BUF_CFG_A + step, 0);
    wreg(IGPU_CURBASE_A + step, 0);

    for (i = 0; i < CURSOR_PAGES; i++) {
        (void)gtt_write(index + i, was[i], NULL);
    }

    wreg(GFX_FLSH_CNTL, 1u);
    (void)reg(G.regs, GFX_FLSH_CNTL);

    say_begin(&line);
    say_text(&line, "intelgpu: cursor test: off again; underrun before ");
    say_text(&line, rep->underrun_before ? "yes" : "no");
    say_text(&line, ", after ");
    say_text(&line, rep->underrun_after ? "yes" : "no");
    say_send(console, &line);
}

void intelgpu_server(long console, long endpoint)
{
    look(console);

    for (;;) {
        static struct message msg, out;
        uint64_t sender = 0;
        struct gpu_request rq;
        struct gpu_reply rep;

        if (kosmos_receive(endpoint, &msg, &sender, 0, 0) != 0) {
            continue;
        }

        memset(&rep, 0, sizeof rep);

        if (msg.length < sizeof rq) {
            rep.error = GPU_ERR_BAD_OP;
        } else {
            memcpy(&rq, msg.data, sizeof rq);

            if (rq.op == GPU_OP_LOOK) {
                look(console);
                rep.error = G.found ? GPU_OK : GPU_ERR_NO_DEVICE;
            } else if (rq.op == GPU_OP_CURSOR_TEST) {
                cursor_test(console, rq.seconds < 1 ? 1 : rq.seconds > 30 ? 30 : rq.seconds, &rep);
            } else {
                rep.error = GPU_ERR_BAD_OP;
            }
        }

        memset(&out, 0, sizeof out);
        out.tag = msg.tag;
        out.length = (uint32_t)sizeof rep;
        memcpy(out.data, &rep, sizeof rep);
        (void)kosmos_reply(sender, &out);
    }
}
