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
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "gpuproto.h"
#include "intelgpu_decode.h"
#include "intelgpu_fill.h"
#include "intelgpu_kernel.h"
#include "intelgpu_lrc.h"
#include "intelgpu_ctx.h"
#include "intelgpu_renderstate.h"
#include "intelgpu_render.h"
#include "intelgpu_blend.h"
#include "kits/gfx/surface.h"          /* over(): the compositor's blend, to hold the GPU's to */
#include "mmio.h"
#include "init/say.h"

/* The GPU's interrupt, when it has one (G7e; `irq_arm`, below). */
static long gpu_irq = -1;                       /* the line's capability */
static bool irq_armed;
static unsigned long irq_count;
static long mocs_console;                   /* defined below, where it is set */
static void irq_say(const char *what);

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

    if (irq_armed) {
        irq_say("intelgpu: the engines' interrupts");
    }

    if (gpu_irq < 0 && dev.intid != 0) {
        gpu_irq = kosmos_irq_claim(dev.intid);
        say_begin(&line);
        say_text(&line, gpu_irq >= 0 ? "intelgpu: its interrupt is vector "
                                     : "intelgpu: its interrupt could not be claimed, vector ");
        say_dec(&line, dev.intid);
        say_text(&line, gpu_irq >= 0 ? "; fences are heard from it" : "; fences are polled");
        say_send(console, &line);
    }

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

/*
 * **The GPU's interrupt** (`docs/gpu3d.md` G7e). Without it this driver
 * heard a fence only when it next looked, on the scheduler's tick - 4 ms -
 * so a frame's work, done by the GPU in a fraction of a millisecond, was
 * heard 8 ms after it was handed over (`testing.md` 18.934). The render
 * and copy engines' user and context-switch interrupts are armed in the
 * GT's first bank, the master control let through, and the server waits on
 * the line and its endpoint at once. The offsets and bits are Linux 6.12's
 * (`i915_reg.h`: GEN8_MASTER_IRQ, GEN8_GT_IMR/IIR/IER(0),
 * GT_RENDER_USER_INTERRUPT, GT_CONTEXT_SWITCH_INTERRUPT, the RCS shift 0
 * and the BCS shift 16), the order its `gen3_irq_init` and
 * `gen8_irq_handler` keep.
 */
#define IGPU_MASTER_IRQ          0x44200u
#define IGPU_MASTER_IRQ_CONTROL  (1u << 31)
#define IGPU_MASTER_GT0          0x3u           /* GEN8_GT_RCS_IRQ | GEN8_GT_BCS_IRQ */
#define IGPU_GT_IMR0             0x44304u
#define IGPU_GT_IIR0             0x44308u
#define IGPU_GT_IER0             0x4430cu
#define IGPU_GT_USER             (1u << 0)
#define IGPU_GT_CONTEXT_SWITCH   (1u << 8)
#define IGPU_GT_WANTED           ((IGPU_GT_USER | IGPU_GT_CONTEXT_SWITCH) \
                                  | (IGPU_GT_USER | IGPU_GT_CONTEXT_SWITCH) << 16)
/* Each engine's own mask, RING_IMR (base + 0xa8, `intel_engine_regs.h`),
 * written as `gen8_logical_ring_enable_irq` writes it - with the bank's
 * shift, so the blitter's bits at 16 - and without it the bank hears
 * nothing however it is armed: IER and IMR as asked and IIR nought after
 * the GPU's work, on the M700. */
#define IGPU_RING_IMR            0xa8u

/* Once, the first time an engine starts - the GT awake, as writes to it
 * need. What is pending is cleared first, twice as Linux does, since IIR
 * can hold two events. */
static void irq_say(const char *what)
{
    struct say_line line;

    say_begin(&line);
    say_text(&line, what);
    say_text(&line, ": IER ");
    say_hex(&line, reg(G.regs, IGPU_GT_IER0), 8);
    say_text(&line, " IMR ");
    say_hex(&line, reg(G.regs, IGPU_GT_IMR0), 8);
    say_text(&line, " IIR ");
    say_hex(&line, reg(G.regs, IGPU_GT_IIR0), 8);
    say_text(&line, " ISR ");
    say_hex(&line, reg(G.regs, IGPU_GT_IMR0 - 4u), 8);
    say_text(&line, " master ");
    say_hex(&line, reg(G.regs, IGPU_MASTER_IRQ), 8);
    say_text(&line, " RCS ");
    say_hex(&line, reg(G.regs, IGPU_RENDER_BASE + IGPU_RING_IMR), 8);
    say_text(&line, " BCS ");
    say_hex(&line, reg(G.regs, IGPU_BLT_BASE + IGPU_RING_IMR), 8);
    say_text(&line, ", ");
    say_dec(&line, irq_count);
    say_text(&line, " heard");
    say_send(mocs_console, &line);
}

static void irq_arm(void)
{
    if (irq_armed || gpu_irq < 0) {
        return;
    }
    wreg(IGPU_GT_IIR0, 0xffffffffu);
    wreg(IGPU_GT_IIR0, 0xffffffffu);
    wreg(IGPU_GT_IER0, IGPU_GT_WANTED);
    wreg(IGPU_GT_IMR0, ~IGPU_GT_WANTED);
    (void)reg(G.regs, IGPU_GT_IMR0);            /* posted */
    wreg(IGPU_RENDER_BASE + IGPU_RING_IMR, ~(IGPU_GT_USER | IGPU_GT_CONTEXT_SWITCH));
    wreg(IGPU_BLT_BASE + IGPU_RING_IMR, ~((IGPU_GT_USER | IGPU_GT_CONTEXT_SWITCH) << 16));
    (void)reg(G.regs, IGPU_BLT_BASE + IGPU_RING_IMR);
    wreg(IGPU_MASTER_IRQ, IGPU_MASTER_IRQ_CONTROL);
    irq_armed = true;
    irq_say("intelgpu: the engines' interrupts armed");
}

/* An interrupt came: the master closed while the bank is read and
 * cleared, then opened again, so one arriving meanwhile raises another.
 * What it was for is not decided here - the loop polls the engines and the
 * contexts after every wake, and those read the fences. */
static void irq_service(void)
{
    static unsigned long stray;
    uint32_t master;

    wreg(IGPU_MASTER_IRQ, 0);
    master = reg(G.regs, IGPU_MASTER_IRQ);
    if ((master & IGPU_MASTER_GT0) != 0) {
        uint32_t iir = reg(G.regs, IGPU_GT_IIR0);

        if (iir != 0) {
            wreg(IGPU_GT_IIR0, iir);
        }
        /* The first, said: that the line is live at all. The count after
         * is `gpu`'s to say (`irq_say`). */
        if (irq_count == 0) {
            struct say_line line;

            say_begin(&line);
            say_text(&line, "intelgpu: the first interrupt, master ");
            say_hex(&line, master, 8);
            say_text(&line, ", bank ");
            say_hex(&line, iir, 8);
            say_text(&line, ", count ");
            say_dec(&line, irq_count + 1u);
            say_send(mocs_console, &line);
        }
    } else if (++stray >= 1000u) {
        /* **A thousand that were not the engines'** - the display's, or
         * something this driver never armed: rather than a storm on a
         * thread in the display band, the master stays closed and the
         * fences are polled again, as before G7e. */
        struct say_line line;

        irq_armed = false;
        (void)kosmos_irq_ack(gpu_irq);
        say_begin(&line);
        say_text(&line, "intelgpu: a thousand interrupts not the engines', master ");
        say_hex(&line, master, 8);
        say_text(&line, " - closed, and fences polled");
        say_send(mocs_console, &line);
        return;
    }
    wreg(IGPU_MASTER_IRQ, IGPU_MASTER_IRQ_CONTROL);
    irq_count++;
    (void)kosmos_irq_ack(gpu_irq);
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

/*
 * **The driver's own page table, the global GTT's mirror** (`docs/gpu3d.md`
 * G6d). On execlists every address an engine is given goes through its
 * context's page table unless the command says the global GTT - a copy's
 * source and destination, a surface's state, a kernel - so the driver's own
 * contexts, the render engine's and the blitter's, are given a page table
 * that maps each GPU address to the page the global GTT maps it to: i915's
 * aliasing PPGTT. Every entry `gtt_write` makes is made here too, and the
 * firmware's framebuffer, which the GTT held before the driver ran, is
 * copied in once.
 *
 * Its pages come in runs of AP_CHUNK_PAGES, contiguous and write-combining.
 * A 4 GB GTT mapped whole would need 2048 tables and six above them, so
 * AP_CHUNKS is the GTT's own size, not a guess.
 */
#define AP_CHUNK_PAGES 64u
#define AP_CHUNKS      ((GTT_ENTRIES / 512u + 6u + AP_CHUNK_PAGES - 1u) / AP_CHUNK_PAGES)

static struct {
    bool              ready, failed;
    struct igpu_ppgtt pt;
    uint8_t          *cpu[AP_CHUNKS];
    uint64_t          phys[AP_CHUNKS];
    unsigned          chunks;
    unsigned          used;               /* pages taken from the last run */
    uint32_t          generation;         /* bumped by every change, for the engines' TLBs */
} AP;

static void *ap_page(void *ctx, uint64_t *phys)
{
    uint8_t *p;

    (void)ctx;

    if (AP.chunks == 0 || AP.used == AP_CHUNK_PAGES) {
        long region, at, base;

        if (AP.chunks == AP_CHUNKS) {
            return NULL;
        }

        region = kosmos_mem_create_flags(AP_CHUNK_PAGES, MEM_CONTIGUOUS | MEM_WRITE_COMBINE);
        if (region < 0 || (at = kosmos_mem_map(region)) < 0 || (base = kosmos_mem_phys(region)) < 0) {
            return NULL;
        }

        AP.cpu[AP.chunks] = (uint8_t *)at;
        AP.phys[AP.chunks] = (uint64_t)base;
        AP.chunks++;
        AP.used = 0;
    }

    p = AP.cpu[AP.chunks - 1u] + AP.used * 4096u;
    *phys = AP.phys[AP.chunks - 1u] + AP.used * 4096u;
    AP.used++;
    memset(p, 0, 4096);
    return p;
}

static void *ap_cpu(void *ctx, uint64_t phys)
{
    unsigned i;

    (void)ctx;

    for (i = 0; i < AP.chunks; i++) {
        if (phys >= AP.phys[i] && phys < AP.phys[i] + AP_CHUNK_PAGES * 4096u) {
            return AP.cpu[i] + (phys - AP.phys[i]);
        }
    }

    return NULL;
}

/* A global GTT entry, made in the mirror too: its address and its PAT
 * index - PWT, PCD and PAT, bits 3, 4 and 7, the same in both kinds of
 * entry - or the page unmapped. */
static void ap_mirror(unsigned long index, uint64_t value)
{
    uint64_t gpu = (uint64_t)index << 12;

    if (!AP.ready) {
        return;
    }

    if ((value & 1u) != 0) {
        unsigned pat = (unsigned)((value >> 3) & 1u) | (unsigned)((value >> 4) & 1u) << 1
                     | (unsigned)((value >> 7) & 1u) << 2;

        if (!igpu_ppgtt_map(&AP.pt, gpu, value & 0x0000fffffffff000ull, pat)) {
            AP.failed = true;
        }
    } else {
        igpu_ppgtt_unmap(&AP.pt, gpu);
    }

    AP.generation++;
}

/* The mirror made, once the GTT can be read: its root, and the entries the
 * firmware's framebuffer already had. */
static void ap_start(void)
{
    struct igpu_pt_pages pages = { ap_page, ap_cpu, NULL };
    unsigned long i, first = G.surface >> 12;

    if (AP.ready || AP.failed) {
        return;
    }

    if (!igpu_ppgtt_init(&AP.pt, &pages)) {
        AP.failed = true;
        return;
    }

    AP.ready = true;

    for (i = 0; G.pipe >= 0 && i < G.fb_pages && first + i < GTT_ENTRIES; i++) {
        uintptr_t p = gtt_window + (first + i) * 8u;

        ap_mirror(first + i, (uint64_t)mmio_read32(p) | (uint64_t)mmio_read32(p + 4) << 32);
    }
}

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

    ap_start();

    p = gtt_window + index * 8u;

    if (was != NULL) {
        *was = (uint64_t)mmio_read32(p) | (uint64_t)mmio_read32(p + 4) << 32;
    }

    mmio_write32(p + 4, (uint32_t)(value >> 32));
    mmio_write32(p, (uint32_t)value);
    ap_mirror(index, value);
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

static struct igpu_slots slots;         /* the GTT's slots past the cursor's (`intelgpu_decode.h`) */

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
    {
        unsigned long next = (((unsigned long)(G.surface >> 12) + G.fb_pages + 255u) & ~255ul);

        igpu_slots_init(&slots, (next + CURSOR_PAGES + 255u) & ~255ul, GTT_ENTRIES);
    }
}

/*--------------------------------------------------------------------------
 * **The engines' work** (`docs/m700-2d.md` step 5, `docs/gpu3d.md` G6d):
 * forcewake, a ring, commands, and the fence they write looked for. Each
 * engine's ring belongs to a context, submitted on execlists - its image
 * in memory says where the ring is and how far to run it, and the submit
 * port is the doorbell (`el_go`). Until G6d the ring was the engine's own,
 * programmed as i915's legacy `xcs_resume` does and rung by moving its
 * tail register; that path is gone.
 *------------------------------------------------------------------------*/

/* Pages of our own, write-combining and in one run, entered in GTT slots
 * past the ones already given out: their address for the processor and for
 * the GPU. */
static bool gtt_pages(unsigned long pages, volatile uint32_t **cpu, uint32_t *gtt)
{
    long region = kosmos_mem_create_flags(pages, MEM_CONTIGUOUS | MEM_WRITE_COMBINE);
    long at, phys;
    unsigned long i, first;

    buffers_init();

    if (region < 0 || (at = kosmos_mem_map(region)) < 0 || (phys = kosmos_mem_phys(region)) < 0
        || !igpu_slots_take(&slots, pages, &first)) {
        return false;
    }

    for (i = 0; i < pages; i++) {
        if (!gtt_write(first + i, (uint64_t)(phys + (long)(i * 4096u)) | 1u, NULL)) {
            return false;
        }
    }

    wreg(GFX_FLSH_CNTL, 1u);
    (void)reg(G.regs, GFX_FLSH_CNTL);

    *cpu = (volatile uint32_t *)at;
    *gtt = (uint32_t)(first << 12);
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
 * **An engine** (`m700-2d.md` 5 and 7a, `gpu3d.md` G6d and G6e): the
 * blitter at 0x22000 and the render engine at 0x2000, two of one thing -
 * the same registers at their own base, started the same way, each with a
 * status page, the driver's own context (a ring, an image, a fence) and
 * programs' contexts taking turns after it. Only the fence's command
 * differs, MI_FLUSH_DW against PIPE_CONTROL (`submit`).
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

    /* Its context (G6d): the image the engine loads and saves, the status
     * buffer's read pointer, and what was last handed to it. */
    volatile uint32_t *image;
    uint32_t          image_gtt;
    unsigned          csb_read;
    bool              active;           /* submitted, and its going idle not yet seen */
    uint32_t          sent;             /* the ring's tail as last submitted, in dwords */
    bool              open;             /* a request begun: commands emitted since submit */
    uint32_t          gtt_seen;         /* the mirror's generation its TLB last saw */
    uint32_t          submissions;      /* contexts submitted, since boot */
    uint64_t          watch_fence;      /* the fence last seen moving, and when */
    unsigned long     watch_since;
    uint32_t          resets;           /* hangs reset, since boot */

    /* Kept across runs (G6e): once the engine has saved the image back,
     * the next submission restores it rather than laying it out again -
     * so neither a program's state nor the desktop's leaks into the other
     * - with these two dwords of its state page written first. */
    bool              own_ran;
    int               own_tail_at, own_control_at;
    uint32_t          running;          /* whose context it was given: 0 the driver's, or a program's id */
    uint32_t          last;             /* whose ran last - so whose state the engine holds - or ~0u */
    unsigned          turn;             /* the program's context looked at first next time */

    /* **The engine's default state** (G6e, i915's `engine->default_state`):
     * the image a context saved having run nothing, recorded once as the
     * engine starts. A program's new context begins from it, restored -
     * never from whatever the last context left in the engine. */
    uint32_t         *golden;
    unsigned          golden_pages;
};

static unsigned long counter_per_ms;    /* the processor's counter, for the hang watch */

static struct engine BCS = { .base = IGPU_BLT_BASE, .name = "the blitter" };
static struct engine RCS = { .base = IGPU_RENDER_BASE, .render = true, .name = "the render engine" };

/*
 * **A program's context on the M700** (G6e): a ring, an image and a fence
 * page in the global GTT, a page table of its own over pages from a pool
 * of its own, and where its ring and its timeline have got to. The
 * stand-in, where there is no Intel GPU, uses the pool and the fence.
 */
struct ptpool {
    long     *caps;
    uint8_t **cpu;
    uint64_t *phys;
    unsigned  n, room, used;            /* chunks, and pages taken from the last */
    bool      wc;                       /* write-combining: the GPU's to walk */
};

struct hwctx {
    struct ptpool      pool;
    volatile uint32_t *ring, *image, *fence;
    uint32_t           ring_gtt, image_gtt, fence_gtt;
    long               ring_cap, image_cap, fence_cap, fence_ro;
    uint32_t           tail, sent;      /* in dwords */
    uint32_t           in_flight;       /* dwords given since it was last idle */
    uint64_t           given;           /* the last value its ring will write */
    bool               ran;             /* its image is the engine's save: restore it */
    int                tail_at, control_at;
};

static struct igpu_ctxs CTXS;

static bool gtt_alloc(unsigned long pages, volatile uint32_t **cpu, uint32_t *gtt, long *cap,
                      unsigned long flags);
static void gtt_free(unsigned long pages, volatile uint32_t *cpu, uint32_t gtt, long cap);
static void golden_capture(struct engine *e);

static uint32_t eng(const struct engine *e, uint32_t offset)
{
    return reg(G.regs, e->base + offset);
}

static void eng_write(const struct engine *e, uint32_t offset, uint32_t value)
{
    wreg(e->base + offset, value);
}

/* `enable_execlists` and `reset_csb_pointers`, for an engine started and
 * for one just reset. */
static bool el_enable(struct engine *e)
{
    unsigned i;

    eng_write(e, IGPU_RING_HWSTAM, ~0u);
    eng_write(e, IGPU_RING_MODE, (IGPU_GFX_RUN_LIST_ENABLE << 16) | IGPU_GFX_RUN_LIST_ENABLE);
    eng_write(e, IGPU_RING_MI_MODE, IGPU_STOP_RING << 16);     /* masked: STOP_RING off */
    eng_write(e, IGPU_RING_HWS_PGA, e->status_gtt);
    (void)eng(e, IGPU_RING_HWS_PGA);

    for (i = 0; i < 2u * IGPU_CSB_ENTRIES; i++) {
        e->status[IGPU_CSB_AT + i] = ~0u;
    }
    e->status[IGPU_CSB_WRITE_AT] = IGPU_CSB_ENTRIES - 1u;
    eng_write(e, IGPU_RING_CONTEXT_STATUS_PTR, 0xffffu << 16 | (IGPU_CSB_ENTRIES - 1u) << 8 | (IGPU_CSB_ENTRIES - 1u));
    (void)eng(e, IGPU_RING_CONTEXT_STATUS_PTR);
    e->csb_read = IGPU_CSB_ENTRIES - 1u;
    e->active = false;
    e->watch_since = 0;
    e->gtt_seen = AP.generation - 1u;           /* the next request invalidates */

    return (eng(e, IGPU_RING_MODE) & IGPU_GFX_RUN_LIST_ENABLE) != 0;
}

static uint32_t engine_start(struct engine *e)
{
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
        || !gtt_pages(1, &e->target, &e->target_gtt)
        || !gtt_pages(e->render ? IGPU_LRC_PAGES_RENDER : IGPU_LRC_PAGES_OTHER, &e->image, &e->image_gtt)) {
        return GPU_ERR_NO_MEMORY;
    }

    /* The mirror its context's addresses go through, which the GTT's
     * first entry made. */
    if (!AP.ready || AP.failed) {
        return GPU_ERR_NO_MEMORY;
    }

    /*
     * **On execlists** (G6d), as the video engine was in G6b - i915's
     * `enable_execlists`: the status page's writes masked, run-list mode,
     * STOP_RING off, the status page; then `reset_csb_pointers`, both at
     * the last entry so the first the engine writes is entry 0. The ring
     * is no longer programmed through the engine's registers: its start,
     * size, head and tail are in the context's image, laid out at every
     * submission (`el_go`).
     */
    e->sent = 0;
    e->open = false;
    e->last = ~0u;

    if (!el_enable(e)) {
        return GPU_ERR_NO_RING;
    }

    golden_capture(e);

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

        if (machine.counter_hz >= 1000u) {
            counter_per_ms = machine.counter_hz / 1000u;
        }
    }

    e->tail = 0;
    e->status[FENCE_AT] = 0;
    e->issued = 0;
    e->next_value = 0x4b6f7301u;                             /* "Kos" and a count */
    e->ready = true;
    irq_arm();
    return GPU_OK;
}

/*
 * **A context's image, ready to be submitted** (G6d, G6e). The first time:
 * laid out afresh with its restore inhibited, as a context never run is -
 * or, for a program's, the engine's default state copied in and restored,
 * so it begins from nothing another context left. After that: the image
 * the engine saved when it went idle, with only its tail and its context
 * control written - restored when another context ran in between, and
 * inhibited when this one was the last, whose state the engine still holds
 * (`igpu_lrc_inhibit_control`). Only ever while the engine is idle: the
 * image is the engine's to write until it says it has gone idle, and
 * writing the tail into it while it is saving would lose the tail.
 */
static bool image_ready(struct engine *e, volatile uint32_t *image, bool *ran, int *tail_at,
                        int *control_at, bool same, bool from_default, const struct igpu_lrc_init *in)
{
    static uint32_t state[1024];
    unsigned i, pages = e->render ? IGPU_LRC_PAGES_RENDER : IGPU_LRC_PAGES_OTHER;
    bool restore = from_default && e->golden != NULL;

    if (*ran) {
        image[IGPU_LRC_STATE_PAGE * 1024u + (unsigned)*tail_at] = in->tail;
        image[IGPU_LRC_STATE_PAGE * 1024u + (unsigned)*control_at] =
            same ? igpu_lrc_inhibit_control() : igpu_lrc_restore_control();
        return true;
    }

    if (restore) {
        memcpy(state, e->golden + IGPU_LRC_STATE_PAGE * 1024u, sizeof state);
        if (!igpu_lrc_patch(state, in, true)) {
            return false;
        }
    } else if (!igpu_lrc_fill(state, e->render ? IGPU_LRC_RENDER : IGPU_LRC_OTHER, in)) {
        return false;
    }

    *tail_at = igpu_lrc_find(state, in->base, 0x030u);         /* RING_TAIL */
    *control_at = igpu_lrc_find(state, in->base, 0x244u);      /* CONTEXT_CONTROL */

    if (*tail_at < 0 || *control_at < 0) {
        return false;
    }

    for (i = 0; i < 1024u; i++) {
        image[i] = 0;                                           /* the per-process status page */
        image[IGPU_LRC_STATE_PAGE * 1024u + i] = state[i];
    }

    for (i = (IGPU_LRC_STATE_PAGE + 1u) * 1024u; restore && i < pages * 1024u; i++) {
        image[i] = e->golden[i];                                /* the engine's own state, by default */
    }

    return true;
}

/* The submit port written with one context's descriptor. */
static void el_submit(struct engine *e, uint32_t image_gtt, uint32_t id)
{
    uint32_t words[4];
    unsigned i;

#if defined(__x86_64__)
    __asm__ volatile("sfence" ::: "memory");    /* the image and the tables out before the port */
#endif

    (void)forcewake(true);
    igpu_elsp_words(igpu_lrc_descriptor(image_gtt, id), 0, words);

    for (i = 0; i < 4u; i++) {
        eng_write(e, IGPU_RING_ELSP, words[i]);
    }

    e->active = true;
    e->submissions++;
}

/*
 * **The engine's default state, recorded** (i915's `__engines_record_defaults`):
 * a context laid out with its restore inhibited, given a ring of two
 * MI_NOOPs, submitted, and its image read back once the engine says it
 * went idle - the state the engine saves having run nothing. Kept in the
 * processor's memory, since reading write-combining pages is slow and a
 * new context's image is copied from it every time. Without it, a
 * program's first run would begin from whatever the context before it
 * left in the engine's registers.
 */
static void golden_capture(struct engine *e)
{
    unsigned pages = e->render ? IGPU_LRC_PAGES_RENDER : IGPU_LRC_PAGES_OTHER, i, n;
    volatile uint32_t *image, *ring;
    uint32_t image_gtt, ring_gtt;
    long image_cap, ring_cap;
    struct igpu_lrc_init in;
    struct igpu_csb_event ev[IGPU_CSB_ENTRIES];
    bool idle = false;
    bool ran = false;
    int tail_at, control_at;

    if (e->golden != NULL || !gtt_alloc(pages, &image, &image_gtt, &image_cap, MEM_WRITE_COMBINE)) {
        return;
    }

    if (!gtt_alloc(1, &ring, &ring_gtt, &ring_cap, MEM_WRITE_COMBINE)) {
        gtt_free(pages, image, image_gtt, image_cap);
        return;
    }

    ring[0] = ring[1] = IGPU_MI_NOOP;
    in.base = e->base;
    in.ring_ggtt = ring_gtt;
    in.ring_pages = 1;
    in.head = 0;
    in.tail = 8;
    in.pml4 = AP.pt.pml4_phys;

    if (image_ready(e, image, &ran, &tail_at, &control_at, false, false, &in)) {
        el_submit(e, image_gtt, 3u);
        e->running = 0;

        for (i = 0; i < 5000 + 50 && !idle; i++) {
            n = igpu_csb_read(e->status, &e->csb_read, ev);
            while (n-- > 0) {
                idle = idle || (ev[n].status & IGPU_CSB_ACTIVE_IDLE) != 0;
            }
            if (!idle && i >= 5000) {
                kosmos_sleep(1);
            }
        }

        e->active = false;
        e->last = ~0u;
    }

    if (idle && (e->golden = malloc(pages * 4096u)) != NULL) {
        for (i = 0; i < pages * 1024u; i++) {
            e->golden[i] = image[i];
        }
        e->golden_pages = pages;
    }

    gtt_free(1, ring, ring_gtt, ring_cap);
    gtt_free(pages, image, image_gtt, image_cap);
}

/* The driver's own context: the desktop's copies and drawing. */
static void el_go(struct engine *e)
{
    struct igpu_lrc_init in;

    in.base = e->base;
    in.ring_ggtt = e->ring_gtt;
    in.ring_pages = RING_PAGES;
    in.head = e->sent * 4u;
    in.tail = e->tail * 4u;
    in.pml4 = AP.pt.pml4_phys;

    if (!image_ready(e, e->image, &e->own_ran, &e->own_tail_at, &e->own_control_at, e->last == 0,
                     false, &in)) {
        return;
    }

    el_submit(e, e->image_gtt, e->render ? 1u : 2u);
    e->own_ran = true;
    e->running = 0;
    e->last = 0;
    e->sent = e->tail;
}

/*
 * A program's context (G6e), submitted the same way under an id of its
 * own - 16 past the driver's, so the status buffer names it apart - with
 * its own page table's root in its image.
 */
#define PROGRAM_CTX_ID 16u

static void hw_go(struct engine *e, uint32_t id)
{
    struct igpu_ctx *c = &CTXS.list[id - 1u];
    struct hwctx *h = c->hw;
    struct igpu_lrc_init in;

    in.base = e->base;
    in.ring_ggtt = h->ring_gtt;
    in.ring_pages = RING_PAGES;
    in.head = h->sent * 4u;
    in.tail = h->tail * 4u;
    in.pml4 = c->pt.pml4_phys;

    if (!image_ready(e, h->image, &h->ran, &h->tail_at, &h->control_at, e->last == id, true, &in)) {
        return;
    }

    el_submit(e, h->image_gtt, PROGRAM_CTX_ID + id);
    h->ran = true;
    h->sent = h->tail;
    e->running = id;
    e->last = id;
}

/* A program's context on this engine with work in its ring, the one after
 * the last served first; 0 for none. */
static uint32_t hw_next(struct engine *e, bool peek)
{
    unsigned k;

    for (k = 0; k < CTXS.n; k++) {
        unsigned i = (e->turn + k) % CTXS.n;
        struct igpu_ctx *c = &CTXS.list[i];
        struct hwctx *h = c->hw;

        if (c->used && h != NULL && h->ring != NULL && (c->engine == GPU_ENGINE_COPY) == !e->render
            && h->sent != h->tail) {
            if (!peek) {
                e->turn = i + 1u;
            }
            return i + 1u;
        }
    }

    return 0;
}

/*
 * **An engine reset** (G6d), as i915 resets one on Gen9 (`intel_reset.c`,
 * Linux 6.12): the ring stopped, which an engine at an arbitration point
 * obeys; a reset asked for in RING_RESET_CTL and its readiness waited for,
 * 700 us at most, or a catastrophic error's bit cleared instead; GDRST
 * given the engine's domain and waited on to clear, twice as i915 does
 * before Gen12.70; the request withdrawn; and execlists turned on again.
 *
 * What was given it and not done is dropped - its fence is written as
 * reached, so nobody waits for it for ever - and said, with the count.
 */
#define GDRST               0x0941cu        /* GEN6_GDRST: not in a power well */
#define GDRST_RENDER        (1u << 1)
#define GDRST_BLT           (1u << 3)
#define RING_RESET_CTL      0xd0u
#define RESET_CTL_REQUEST   (1u << 0)
#define RESET_CTL_READY     (1u << 1)
#define RESET_CTL_CAT_ERROR (1u << 2)
#define RING_MODE_IDLE      (1u << 9)       /* in MI_MODE */

static long mocs_console;                   /* where the driver says things; set at start */
static bool rs_done;                        /* the render engine's starting state run */

static void engine_reset(struct engine *e)
{
    struct say_line line;
    uint32_t ack, request = 0, mask = 0, want = 0, domain = e->render ? GDRST_RENDER : GDRST_BLT;
    struct igpu_ctx *program = e->running != 0 ? &CTXS.list[e->running - 1u] : NULL;
    struct hwctx *ph = program != NULL ? program->hw : NULL;
    uint64_t lost = program != NULL ? ph->given - igpu_ctx_reached(program)
                                    : (uint64_t)(e->issued - e->status[FENCE_AT]);
    bool stopped, ready = true, cleared = true;
    unsigned i, k;

    (void)forcewake(true);

    eng_write(e, IGPU_RING_MI_MODE, IGPU_STOP_RING << 16 | IGPU_STOP_RING);
    for (i = 0; i < 20000 && (eng(e, IGPU_RING_MI_MODE) & RING_MODE_IDLE) == 0; i++) {
    }
    stopped = (eng(e, IGPU_RING_MI_MODE) & RING_MODE_IDLE) != 0;

    ack = eng(e, RING_RESET_CTL);
    if ((ack & RESET_CTL_CAT_ERROR) != 0) {
        request = mask = RESET_CTL_CAT_ERROR;
        want = 0;
    } else if ((ack & RESET_CTL_READY) == 0) {
        request = RESET_CTL_REQUEST;
        mask = want = RESET_CTL_READY;
    }

    if (request != 0) {
        eng_write(e, RING_RESET_CTL, request << 16 | request);
        for (i = 0; i < 20000 && (eng(e, RING_RESET_CTL) & mask) != want; i++) {
        }
        ready = (eng(e, RING_RESET_CTL) & mask) == want;
    }

    for (k = 0; k < 2u; k++) {
        wreg(GDRST, domain);
        for (i = 0; i < 20000 && (reg(G.regs, GDRST) & domain) != 0; i++) {
        }
        for (i = 0; i < 500 && (reg(G.regs, GDRST) & domain) != 0; i++) {
            kosmos_sleep(1);
        }
        cleared = cleared && (reg(G.regs, GDRST) & domain) == 0;
    }

    eng_write(e, RING_RESET_CTL, RESET_CTL_REQUEST << 16);

    /*
     * The work of the context that hung is dropped and its timeline written
     * as reached, so nobody waits for it for ever - a program's counted as
     * its hang, which CONTEXT_STATE says - and its image, which the engine
     * was saving or never saved, laid out again the next time. The other
     * contexts' images are as the engine last saved them.
     */
    if (program != NULL) {
        ph->sent = ph->tail;
        ph->in_flight = 0;
        ph->ran = false;
        ph->fence[1] = (uint32_t)(ph->given >> 32);
        ph->fence[0] = (uint32_t)ph->given;
        program->hangs++;
    } else {
        e->sent = e->tail;
        e->open = false;
        e->status[FENCE_AT] = e->issued;
        e->own_ran = false;

        if (e->render) {
            rs_done = false;               /* the starting state is the context's, and went with it */
        }
    }

    e->resets++;
    e->last = ~0u;                          /* the engine holds nobody's state now */

    (void)el_enable(e);

    say_begin(&line);
    say_text(&line, "intelgpu: ");
    say_text(&line, e->name);
    if (program != NULL) {
        say_text(&line, ", in program context ");
        say_dec(&line, e->running);
        say_text(&line, ",");
    }
    say_text(&line, " hung - its fence still for ");
    say_dec(&line, GPU_HANG_MS);
    say_text(&line, " ms - and was reset (");
    say_dec(&line, e->resets);
    say_text(&line, e->resets == 1 ? " time" : " times");
    say_text(&line, stopped ? "; it stopped" : "; it did not stop");
    say_text(&line, ready ? ", was ready" : ", was not ready");
    say_text(&line, cleared ? ", came out of reset" : ", did not come out of reset");
    say_text(&line, "); ");
    say_dec(&line, lost);
    say_text(&line, lost == 1 ? " request dropped, mode " : " requests dropped, mode ");
    say_hex(&line, eng(e, IGPU_RING_MODE), 8);
    say_send(mocs_console, &line);
}

/* The hang watch: work given, and the fence of the context the engine
 * runs - the driver's own or a program's - not moved for GPU_HANG_MS. */
static void el_watch(struct engine *e)
{
    uint64_t fence, want;
    unsigned long now;

    if (!e->active || counter_per_ms == 0) {
        e->watch_since = 0;
        return;
    }

    if (e->running != 0) {
        struct igpu_ctx *c = &CTXS.list[e->running - 1u];

        fence = igpu_ctx_reached(c);
        want = ((struct hwctx *)c->hw)->given;
    } else {
        fence = e->status[FENCE_AT];
        want = e->issued;
    }

    if (fence == want) {
        e->watch_since = 0;
        return;
    }

    now = kosmos_ticks();

    if (e->watch_since == 0 || fence != e->watch_fence) {
        e->watch_fence = fence;
        e->watch_since = now;
    } else if (now - e->watch_since > (unsigned long)GPU_HANG_MS * counter_per_ms) {
        engine_reset(e);
    }
}

/* The status buffer read: the engine gone idle is what lets the next
 * submission go - and work given while it ran goes now. */
static void el_poll(struct engine *e)
{
    struct igpu_csb_event ev[IGPU_CSB_ENTRIES];
    unsigned i, n;

    if (!e->ready) {
        return;
    }

    n = igpu_csb_read(e->status, &e->csb_read, ev);

    for (i = 0; i < n; i++) {
        if ((ev[i].status & IGPU_CSB_ACTIVE_IDLE) != 0) {
            e->active = false;
        }
    }

    el_watch(e);

    /* The driver's own work first - the desktop's - then a program's. */
    if (!e->active && !e->open) {
        uint32_t id;

        if (e->sent != e->tail) {
            el_go(e);
        } else if ((id = hw_next(e, false)) != 0) {
            hw_go(e, id);
        }
    }
}

/* Whether an engine has work handed it and not yet sent, which the
 * driver's loop comes back for within a tick rather than waiting for the
 * next request. */
static bool el_waiting(struct engine *e)
{
    return e->ready && ((e->sent != e->tail && !e->open) || hw_next(e, true) != 0);
}

/* The fence the engine has reached - its status buffer read first, so work
 * waiting for it to go idle is sent. */
static uint32_t fence_reached(struct engine *e)
{
    el_poll(e);
    return e->status[FENCE_AT];
}

/* Whether everything given has been done - and if so the GT let go, and
 * the time it worked added to its count. */
static bool engine_quiet(struct engine *e)
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

        /* Not while a program's context runs: its work is not the
         * driver's fence's to say. */
        if (engine_quiet(&BCS) && engine_quiet(&RCS) && !(BCS.active && BCS.running != 0)
            && !(RCS.active && RCS.running != 0)) {
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
static void ring_put(struct engine *e, const uint32_t *dw, uint32_t n)
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

/*
 * A request's commands. The first of a request begins with the engine's
 * TLB invalidated when the mirror changed since it last was - a page the
 * driver unmapped and mapped again must not be reached through what the
 * engine remembered of it. On the render engine a PIPE_CONTROL with a
 * post-sync write, without which the invalidation is not sure to happen; on
 * the blitter MI_FLUSH_DW's own, with a store to the same scratch qword
 * (i915's `gen8_emit_flush_rcs` and `_xcs`).
 */
#define IGPU_MI_INVALIDATE_TLB (1u << 18)

static void emit(struct engine *e, const uint32_t *dw, uint32_t n)
{
    if (!e->open) {
        e->open = true;

        if (e->gtt_seen != AP.generation) {
            e->gtt_seen = AP.generation;

            if (e->render) {
                const uint32_t pc[6] = { IGPU_PIPE_CONTROL,
                                         IGPU_PC_CS_STALL | IGPU_PC_TLB_INVALIDATE | IGPU_PC_QW_WRITE
                                             | IGPU_PC_GLOBAL_GTT,
                                         e->status_gtt + SCRATCH_AT * 4u, 0, 0, 0 };
                ring_put(e, pc, 6);
            } else {
                const uint32_t fl[4] = { IGPU_MI_FLUSH_DW_STORE | IGPU_MI_INVALIDATE_TLB,
                                         (e->status_gtt + SCRATCH_AT * 4u) | IGPU_MI_FLUSH_DW_GTT, 0, 0 };
                ring_put(e, fl, 4);
            }
        }
    }

    ring_put(e, dw, n);
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
    e->open = false;
    el_poll(e);                                 /* sent now if the engine is idle */
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
 * **A hang, made on purpose** (G6d): a batch of two commands - MI_ARB_CHECK,
 * where the engine may be stopped, and MI_BATCH_BUFFER_START back to
 * itself, i915's spinner - given as a request, with a store after it the
 * spinner never lets run. Then nothing but the driver's own watch, polled
 * as every wait polls it, until it resets the engine; then a store of its
 * own, the engine working again.
 */
#define IGPU_MI_ARB_CHECK (0x05u << 23)

static void hang_test(long console, struct engine *e, struct gpu_reply *rep)
{
    static volatile uint32_t *spin;
    static uint32_t spin_gtt;
    uint32_t error, before, value;
    unsigned long t0, t1;
    unsigned i;

    (void)console;

    if (!G.found) {
        rep->error = GPU_ERR_NO_DEVICE;
        return;
    }

    if ((error = engine_start(e)) != GPU_OK) {
        rep->error = error;
        return;
    }

    if (spin == NULL && !gtt_pages(1, &spin, &spin_gtt)) {
        rep->error = GPU_ERR_NO_MEMORY;
        return;
    }

    for (i = 0; i < 100 && !engine_idle(e); i++) {
        kosmos_sleep(1);
    }

    if (!engine_idle(e)) {
        rep->error = GPU_ERR_BUSY;
        return;
    }

    spin[0] = IGPU_MI_ARB_CHECK;
    spin[1] = IGPU_MI_BB_START;
    spin[2] = spin_gtt;
    spin[3] = 0;

    {
        const uint32_t start[4] = { IGPU_MI_BB_START, spin_gtt, 0, IGPU_MI_NOOP };
        const uint32_t store[4] = { IGPU_MI_STORE_DWORD | IGPU_MI_USE_GGTT, e->target_gtt, 0, 0x4b6f7348u };

        e->target[0] = 0;
#if defined(__x86_64__)
        __asm__ volatile("sfence" ::: "memory");
#endif
        emit(e, start, 4);
        emit(e, store, 4);
        before = e->resets;
        t0 = kosmos_ticks();
        (void)submit(e);
    }

    for (i = 0; i < (GPU_HANG_MS + 3000u) / 4u && e->resets == before; i++) {
        (void)fence_reached(e);
        kosmos_sleep(1);
    }

    t1 = kosmos_ticks();
    rep->engine.head = e->resets;
    rep->engine.tail = counter_per_ms != 0 ? (uint32_t)((t1 - t0) / counter_per_ms) : 0;

    if (e->resets == before) {
        rep->error = GPU_ERR_NO_ANSWER;
        return;
    }

    /* And the engine after: one store, through a context laid out again. */
    {
        uint32_t dw[4] = { IGPU_MI_STORE_DWORD | IGPU_MI_USE_GGTT, e->target_gtt, 0, 0 };

        value = e->next_value++;
        dw[3] = value;
        e->target[0] = 0;
        emit(e, dw, 4);
        (void)submit(e);

        for (i = 0; i < 20000 && e->target[0] != value; i++) {
            __asm__ volatile("" ::: "memory");
        }
        for (i = 0; i < 100 && e->target[0] != value; i++) {
            (void)fence_reached(e);
            kosmos_sleep(1);
        }
    }

    rep->engine.value = e->target[0];
    rep->engine.mode = eng(e, IGPU_RING_MODE);
    rep->engine.esr = eng(e, IGPU_RING_ESR);
    rep->error = e->target[0] == value ? GPU_OK : GPU_ERR_NO_ANSWER;

    for (i = 0; i < 100 && !engine_idle(e); i++) {
        kosmos_sleep(1);
    }
}

/*
 * **One context on the video engine** (`docs/gpu3d.md` G6b): execlists
 * turned on for VCS0, the one engine nobody uses on the desktop, and one
 * context run there - its image laid out by `intelgpu_lrc.c`, a page table
 * of its own, and in its ring one `MI_STORE_DWORD_IMM` to a GPU address
 * that only that page table maps. The render engine and the blitter stay
 * on their rings, so whatever happens here the desktop draws as before.
 *
 * The sequence is i915's (Linux 6.12): `enable_execlists` - the status
 * page's writes unmasked, run-list mode, STOP_RING off, the status page -
 * then `reset_csb_pointers`, then a descriptor written to the submit port,
 * element 1 first and element 0 last, the last write submitting. A context
 * never run is submitted with its restore inhibited, so the engine starts
 * from the image rather than from anything it held; this test lays the
 * image out afresh for every run, from where the last one's ring ended.
 */
#define FORCEWAKE_MEDIA      0x0a270u    /* FORCEWAKE_MEDIA_GEN9: 0x12000-0x13fff are its */
#define FORCEWAKE_ACK_MEDIA  0x00d88u    /* FORCEWAKE_ACK_MEDIA_GEN9 */
#define PRIVATE_PAT_LO       0x040e0u    /* GEN8_PRIVATE_PAT_LO and _HI */
#define PRIVATE_PAT_HI       0x040e4u
#define EL_TARGET            0x0000123456780000ull  /* past 4 GB, in this page table only */
#define EL_TABLES            8u          /* a PML4, a PDP, a PD and a PT, and room */
#define EL_RING_DWORDS       1024u       /* one page */

/* A context of the test's: its image, its ring, its page table and the
 * page its one address is mapped to. */
struct el_context {
    volatile uint32_t *image, *ring, *target;
    uint32_t           image_gtt, ring_gtt;
    uint8_t           *pool;              /* the page table's pages, then the target */
    uint64_t           pool_phys;
    unsigned           pool_used;
    struct igpu_ppgtt  pt;
    uint32_t           tail;              /* in dwords */
};

#define EL_CONTEXTS 2u

static struct {
    bool               ready;
    volatile uint32_t *status;
    uint32_t           status_gtt;
    struct el_context  c[EL_CONTEXTS];
    unsigned           csb_read;
    uint32_t           next;
    uint32_t           pat_before;        /* PRIVATE_PAT_LO as the firmware left it */
} EL;

static void *el_page(void *ctx, uint64_t *phys)
{
    struct el_context *c = ctx;
    uint8_t *p;

    if (c->pool_used == EL_TABLES) {
        return NULL;
    }

    p = c->pool + c->pool_used * 4096u;
    memset(p, 0, 4096);
    *phys = c->pool_phys + c->pool_used * 4096u;
    c->pool_used++;
    return p;
}

static void *el_cpu(void *ctx, uint64_t phys)
{
    struct el_context *c = ctx;

    return c->pool + (phys - c->pool_phys);
}

static bool media_awake;

/* The media domain kept awake, as `forcewake` keeps the GT's. */
static bool forcewake_media(bool on)
{
    unsigned i;

    if (media_awake == on) {
        return true;
    }

    wreg(FORCEWAKE_MEDIA, (IGPU_FORCEWAKE_KERNEL << 16) | (on ? IGPU_FORCEWAKE_KERNEL : 0u));

    for (i = 0; i < 5000 + 50; i++) {
        if (((reg(G.regs, FORCEWAKE_ACK_MEDIA) & IGPU_FORCEWAKE_KERNEL) != 0) == on) {
            media_awake = on;
            return true;
        }

        if (i >= 5000) {
            kosmos_sleep(1);
        }
    }

    return false;
}

static uint32_t vcs(uint32_t offset)
{
    return reg(G.regs, IGPU_VIDEO_BASE + offset);
}

static void vcs_write(uint32_t offset, uint32_t value)
{
    wreg(IGPU_VIDEO_BASE + offset, value);
}

static uint32_t execlist_start(long console)
{
    struct say_line line;
    long region, at, phys;
    uint64_t pat;
    unsigned i;

    if (EL.ready) {
        return GPU_OK;
    }

    if (!gtt_pages(1, &EL.status, &EL.status_gtt)) {
        return GPU_ERR_NO_MEMORY;
    }

    /* Each context with its own image, ring and page table - and in each
     * page table the same GPU address, EL_TARGET, on a page of its own. */
    for (i = 0; i < EL_CONTEXTS; i++) {
        struct el_context *c = &EL.c[i];
        struct igpu_pt_pages pages = { el_page, el_cpu, c };

        if (!gtt_pages(IGPU_LRC_PAGES_OTHER, &c->image, &c->image_gtt) || !gtt_pages(1, &c->ring, &c->ring_gtt)) {
            return GPU_ERR_NO_MEMORY;
        }

        region = kosmos_mem_create_flags(EL_TABLES + 1u, MEM_CONTIGUOUS | MEM_WRITE_COMBINE);
        if (region < 0 || (at = kosmos_mem_map(region)) < 0 || (phys = kosmos_mem_phys(region)) < 0) {
            return GPU_ERR_NO_MEMORY;
        }

        c->pool = (uint8_t *)at;
        c->pool_phys = (uint64_t)phys;
        c->target = (volatile uint32_t *)(c->pool + EL_TABLES * 4096u);
        c->tail = 0;

        if (!igpu_ppgtt_init(&c->pt, &pages)
            || !igpu_ppgtt_map(&c->pt, EL_TARGET, c->pool_phys + EL_TABLES * 4096u, IGPU_PAT_UC)) {
            return GPU_ERR_NO_MEMORY;
        }
    }

    /* i915's private PAT, but its first entry as the firmware left it: the
     * global GTT always uses index 0, and the desktop is drawn through it. */
    EL.pat_before = reg(G.regs, PRIVATE_PAT_LO);
    pat = igpu_private_pat();
    wreg(PRIVATE_PAT_LO, ((uint32_t)pat & ~0xffu) | (EL.pat_before & 0xffu));
    wreg(PRIVATE_PAT_HI, (uint32_t)(pat >> 32));

    /* `enable_execlists`. */
    vcs_write(IGPU_RING_HWSTAM, ~0u);
    vcs_write(IGPU_RING_MODE, (IGPU_GFX_RUN_LIST_ENABLE << 16) | IGPU_GFX_RUN_LIST_ENABLE);
    vcs_write(IGPU_RING_MI_MODE, IGPU_STOP_RING << 16);
    vcs_write(IGPU_RING_HWS_PGA, EL.status_gtt);
    (void)vcs(IGPU_RING_HWS_PGA);

    /* `reset_csb_pointers`: both pointers at the last entry, so the first
     * the engine writes is entry 0; the entries all ones, so an entry the
     * engine never wrote is seen. */
    for (i = 0; i < 2u * IGPU_CSB_ENTRIES; i++) {
        EL.status[IGPU_CSB_AT + i] = ~0u;
    }
    EL.status[IGPU_CSB_WRITE_AT] = IGPU_CSB_ENTRIES - 1u;
    vcs_write(IGPU_RING_CONTEXT_STATUS_PTR, 0xffffu << 16 | (IGPU_CSB_ENTRIES - 1u) << 8 | (IGPU_CSB_ENTRIES - 1u));
    (void)vcs(IGPU_RING_CONTEXT_STATUS_PTR);
    EL.csb_read = IGPU_CSB_ENTRIES - 1u;

    say_begin(&line);
    say_text(&line, "intelgpu: the video engine on execlists: mode ");
    say_hex(&line, vcs(IGPU_RING_MODE), 8);
    say_text(&line, ", status page at GTT ");
    say_hex(&line, EL.status_gtt, 8);
    say_text(&line, ", private PAT ");
    say_hex(&line, EL.pat_before, 8);
    say_text(&line, " -> ");
    say_hex(&line, reg(G.regs, PRIVATE_PAT_HI), 8);
    say_hex(&line, reg(G.regs, PRIVATE_PAT_LO), 8);
    say_text(&line, ", page table roots ");
    say_hex(&line, (uint32_t)EL.c[0].pt.pml4_phys, 8);
    say_text(&line, " and ");
    say_hex(&line, (uint32_t)EL.c[1].pt.pml4_phys, 8);
    say_send(console, &line);

    if ((vcs(IGPU_RING_MODE) & IGPU_GFX_RUN_LIST_ENABLE) == 0) {
        return GPU_ERR_NO_RING;
    }

    EL.next = 0x4b6f7345u;                   /* "KosE" and a count */
    EL.ready = true;
    return GPU_OK;
}

/* A context's command - a dword to EL_TARGET through its own page table,
 * MI_STORE_DWORD_IMM without MI_USE_GGTT - and its image laid out afresh,
 * the ring from where its last run ended. Answers its descriptor, or 0. */
static uint64_t el_prepare(struct el_context *c, uint32_t value, uint64_t pml4)
{
    static uint32_t state_page[1024];
    struct igpu_lrc_init in;
    uint32_t head;
    unsigned i;

    if (c->tail + 4u > EL_RING_DWORDS) {
        while (c->tail < EL_RING_DWORDS) {
            c->ring[c->tail++] = IGPU_MI_NOOP;
        }
        c->tail = 0;
    }

    head = c->tail;
    c->target[0] = 0;
    c->ring[c->tail++] = IGPU_MI_STORE_DWORD;
    c->ring[c->tail++] = (uint32_t)EL_TARGET;
    c->ring[c->tail++] = (uint32_t)(EL_TARGET >> 32);
    c->ring[c->tail++] = value;

    in.base = IGPU_VIDEO_BASE;
    in.ring_ggtt = c->ring_gtt;
    in.ring_pages = 1;
    in.head = head * 4u;
    in.tail = (c->tail % EL_RING_DWORDS) * 4u;
    in.pml4 = pml4;

    if (!igpu_lrc_fill(state_page, IGPU_LRC_OTHER, &in)) {
        return 0;
    }

    for (i = 0; i < 1024u; i++) {
        c->image[i] = 0;                                    /* the per-process status page */
        c->image[IGPU_LRC_STATE_PAGE * 1024u + i] = state_page[i];
    }

    return igpu_lrc_descriptor(c->image_gtt, value & 0xffffu);
}

/*
 * `contexts` of them, one or two, submitted together: the second in the
 * port's element 1, so the engine runs element 0 and switches to it. Two
 * is G6c: the same GPU address in both page tables, a different page
 * behind each - so each page holding its own context's number and not the
 * other's says the page tables kept them apart. `shared` is its control:
 * both contexts given the first one's page table, so both numbers land on
 * the first page and the second page holds neither - which the test must
 * call a failure.
 */
static void execlist_test(long console, unsigned contexts, bool shared, struct gpu_reply *rep)
{
    struct igpu_csb_event ev[IGPU_CSB_ENTRIES];
    struct say_line line;
    uint32_t value[EL_CONTEXTS] = { 0, 0 }, error, words[4], last = 0, seen = 0;
    uint64_t desc[EL_CONTEXTS] = { 0, 0 };
    unsigned long t0, t1;
    unsigned i, k, events = 0, n;
    bool all;

    if (!G.found) {
        rep->error = GPU_ERR_NO_DEVICE;
        return;
    }

    if (contexts < 1 || contexts > EL_CONTEXTS) {
        rep->error = GPU_ERR_BAD_OP;
        return;
    }

    if (!forcewake(true) || !forcewake_media(true)) {
        rep->error = GPU_ERR_NO_WAKE;
        return;
    }

    if ((error = execlist_start(console)) != GPU_OK) {
        rep->error = error;
        (void)forcewake_media(false);
        return;
    }

    for (k = 0; k < contexts; k++) {
        value[k] = EL.next++;
        if ((desc[k] = el_prepare(&EL.c[k], value[k], EL.c[shared ? 0 : k].pt.pml4_phys)) == 0) {
            rep->error = GPU_ERR_BAD_OP;
            return;
        }
    }

    igpu_elsp_words(desc[0], desc[1], words);

#if defined(__x86_64__)
    __asm__ volatile("sfence" ::: "memory");    /* the images and rings out before the doorbell */
#endif

    t0 = kosmos_ticks();
    for (i = 0; i < 4; i++) {
        vcs_write(IGPU_RING_ELSP, words[i]);
    }

    for (i = 0, all = false; i < 20000 + 100 && !all; i++) {
        all = true;
        for (k = 0; k < contexts; k++) {
            all = all && EL.c[k].target[0] == value[k];
        }
        if (i >= 20000) {
            kosmos_sleep(1);
        }
        __asm__ volatile("" ::: "memory");
    }
    t1 = kosmos_ticks();

    /* And the engine's own account, in the status buffer. */
    for (i = 0; i < 100; i++) {
        n = igpu_csb_read(EL.status, &EL.csb_read, ev);
        for (k = 0; k < n; k++) {
            last = ev[k].status;
            seen |= ev[k].status;
            events++;
        }
        if ((last & IGPU_CSB_ACTIVE_IDLE) != 0) {
            break;
        }
        kosmos_sleep(1);
    }

    {
        static struct sysinfo machine;
        unsigned long per_us = kosmos_sysinfo(&machine) == 0 && machine.counter_hz >= 1000000u
                             ? machine.counter_hz / 1000000u : 1u;

        rep->engine.elapsed_us = (uint32_t)((t1 - t0) / per_us);
    }

    rep->engine.value = EL.c[0].target[0];
    rep->engine.head = contexts > 1 ? EL.c[1].target[0] : 0;
    rep->engine.tail = vcs(IGPU_RING_EXECLIST_STATUS);
    rep->engine.ctl = events << 16 | (seen & 0xffffu);
    rep->engine.mode = vcs(IGPU_RING_MODE);
    rep->engine.esr = vcs(IGPU_RING_ESR);
    rep->error = all ? GPU_OK : GPU_ERR_NO_ANSWER;

    say_begin(&line);
    say_text(&line, "intelgpu: the video engine, ");
    say_dec(&line, contexts);
    say_text(&line, contexts > 1 ? " contexts at GPU address 123456780000: " : " context at GPU address 123456780000: ");
    for (k = 0; k < contexts; k++) {
        say_text(&line, k > 0 ? ", " : "");
        say_hex(&line, EL.c[k].target[0], 8);
        say_text(&line, EL.c[k].target[0] == value[k] ? " (its own)" : " (wanted ");
        if (EL.c[k].target[0] != value[k]) {
            say_hex(&line, value[k], 8);
            say_text(&line, ")");
        }
    }
    say_text(&line, "; ");
    say_dec(&line, events);
    say_text(&line, " status events, the last ");
    say_hex(&line, last, 8);
    say_text(&line, ", all ");
    say_hex(&line, seen, 8);
    say_text(&line, ", execlist status ");
    say_hex(&line, rep->engine.tail, 8);
    say_text(&line, ", head ");
    say_hex(&line, vcs(IGPU_RING_HEAD), 8);
    say_text(&line, " esr ");
    say_hex(&line, rep->engine.esr, 8);
    say_send(console, &line);

    (void)forcewake_media(false);
    if (engine_quiet(&BCS) && engine_quiet(&RCS)) {
        (void)forcewake(false);
    }
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
 * buffers are cached in the LLC and not in the L3 (`run_surface`), so what
 * the processor wrote is what the kernel reads and what it writes is
 * there for the processor when the fence comes.
 */
#define RUN_CURBE_AT  0x000u
#define RUN_IDD_AT    0x100u
#define RUN_BT_AT     0x140u
#define RUN_SS_AT     0x180u
#define RUN_THREADS   168u              /* the HD 530's 24 EUs, seven threads each */

#define SURFACEFORMAT_B8G8R8A8_UNORM 0x0c0u   /* isl_format.h */
#define SURFACEFORMAT_R32_FLOAT      0x0d8u
#define SURFACEFORMAT_R32_UINT       0x0d7u   /* isl.h: 215 */
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
    } else if (view == GPU_VIEW_TABLE) {
        /* A buffer of words the sampler reads with ld: its entries less
         * one spread over width, height and depth as a buffer's are, each
         * entry four bytes (`testing.md` 18.705). */
        uint32_t last = b->pitch * b->height / 4u - 1u;

        ss[0] = (4u << 29) | (SURFACEFORMAT_R32_UINT << 18) | (1u << 8);
        ss[2] = (last & 0x7fu) | (((last >> 7) & 0x3fffu) << 16);
        ss[3] = (((last >> 21) & 0x3fu) << 21) | 3u;     /* the pitch: four bytes, less one */
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

    /* MOCS index 2, write-back in the LLC and uncached in the GPU's L3
     * (`surface_mocs`), where they had been uncached in both. The LLC is
     * kept in step with the processor's caches, so what it wrote is what
     * the kernel reads and what the kernel wrote is what it reads after the
     * fence. The L3 is not, and caching there as well bought Cafesa3D's
     * tracer nothing - 132 ms a pass either way - so it stays out
     * (`testing.md` 18.700). */
    ss[1] = (2u << 1) << 24;
    ss[7] = (4u << 25) | (5u << 22) | (6u << 19) | (7u << 16);   /* R, G, B, A */
    ss[8] = b->gtt;
    return true;
}

/*
 * **The GT asked for its full clock** while it computes. Nothing had asked
 * it for any, and it ran a kernel at 100 MHz of its 350 to 1150 - Cafesa3D's
 * tracer a pass in 1.1 s (`testing.md` 18.700). RP0, the most the part
 * says it does (RP_STATE_CAP 7:0, in 50 MHz), asked as i915's
 * `gen6_rps_set` asks on generation 9: GEN6_RPNSWREQ, in 50/3 MHz from bit
 * 23. Between runs the GT still falls asleep in RC6, so the request costs
 * nothing while there is no work. Said once, with what the GT then reads.
 */
static bool rps_asked;
static bool mocs_done;
static void mocs_program(long console);

static void rps_up(long console)
{
    struct say_line line;
    uint32_t rp0 = reg(G.regs, IGPU_RP_STATE_CAP) & 0xffu;
    unsigned i;

    if (rps_asked || rp0 == 0) {
        return;
    }

    rps_asked = true;
    wreg(IGPU_RPNSWREQ, (rp0 * 3u) << 23);

    for (i = 0; i < 20; i++) {
        if (((reg(G.regs, IGPU_RPSTAT1) >> 23) & 0x1ffu) == rp0 * 3u) {
            break;
        }

        kosmos_sleep(1);
    }

    say_begin(&line);
    say_text(&line, "intelgpu: the GT asked for ");
    say_dec(&line, rp0 * 50u);
    say_text(&line, " MHz, and running at ");
    say_dec(&line, ((reg(G.regs, IGPU_RPSTAT1) >> 23) & 0x1ffu) * 50u / 3u);
    say_text(&line, " MHz");
    say_send(console, &line);
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
            || q->buffers[i].view > GPU_VIEW_TABLE) {
            return GPU_ERR_NO_BUFFER;
        }

        if (B[id].holder != who) {
            return GPU_ERR_NOT_YOURS;
        }
    }

    if (!forcewake(true)) {
        return GPU_ERR_NO_WAKE;
    }

    rps_up(mocs_console);

    /* The cache controls a run's surfaces name, programmed here too: the
     * 3D path did it, and a machine that never drew had them as a reset
     * leaves them. */
    if (!mocs_done) {
        mocs_program(mocs_console);
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
        /* IEEE, and not single program flow: with bit 18 set the hardware
         * keeps no lane's mask through if, else and while, and a thread
         * took a branch whole when any of its lanes did - Cafesa3D's tracer
         * drew black on the M700 while the emulator, which masks lanes as
         * the branches say, drew the processor's picture (18.700). */
        idd[2] = 0;
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
/*
 * **A list of runs in one submission** (GPU_OP_RUNS, `cafesa3d-gpu.md` R8
 * W2): each step as `run` makes one, one after the other, and a stall with
 * the data port's lines flushed between them so each reads what the last
 * wrote. The buffers' surfaces are shared; each step has its own constants
 * - the shared ones with its number at `step_at` - and its own interface
 * descriptor, in a state area of four pages: the surfaces in the first,
 * the constants from 0x1000 (256 bytes a step) and the descriptors from
 * 0x3000 (32 bytes a step). Its own base addresses a step, so a kernel's
 * instructions are still its own pages and nothing else. Timed as one:
 * the clock before the first walker and after the last stall.
 */
#define RUNS_PAGES     4u
#define RUNS_CURBE_AT  0x1000u
#define RUNS_IDD_AT    0x3000u

static volatile uint32_t *runs_state;
static uint32_t runs_state_gtt;

static uint32_t runs(const struct gpu_runs *q, struct gpu_reply *rep)
{
    const uint32_t who = holder_of_request();
    uint32_t error, i, j, regs;

    if (!G.found) {
        return GPU_ERR_NO_DEVICE;
    }

    if (q->nsteps == 0 || q->nsteps > GPU_RUN_STEPS || q->nbuffers > GPU_RUN_BUFFERS
        || q->nconstants > GPU_RUN_CONSTANTS || q->nconstants % 32u != 0
        || q->step_at % 4u != 0 || q->step_at + 4u > q->nconstants) {
        return GPU_ERR_BAD_OP;
    }

    for (i = 0; i < q->nsteps; i++) {
        const struct kernel *k;
        uint32_t id = q->steps[i].kernel;

        if (id == 0 || id >= KERNELS_MAX || !K[id].used
            || (q->steps[i].simd != 8u && q->steps[i].simd != 16u)
            || q->steps[i].groups_x == 0 || q->steps[i].groups_y == 0
            || q->steps[i].groups_x > 65535u || q->steps[i].groups_y > 65535u) {
            return GPU_ERR_BAD_OP;
        }

        k = &K[id];

        if (k->holder != who || who == 0) {
            return GPU_ERR_NOT_YOURS;
        }

        if ((int32_t)q->nbuffers <= k->bti_max) {
            return GPU_ERR_BAD_OP;
        }
    }

    for (i = 0; i < q->nbuffers; i++) {
        uint32_t id = q->buffers[i].id;

        if (id == 0 || id >= BUFFERS_MAX || !B[id].used || B[id].kind != GPU_BUFFER_IMPORTED
            || q->buffers[i].view > GPU_VIEW_TABLE) {
            return GPU_ERR_NO_BUFFER;
        }

        if (B[id].holder != who) {
            return GPU_ERR_NOT_YOURS;
        }
    }

    if (!forcewake(true)) {
        return GPU_ERR_NO_WAKE;
    }

    if (!mocs_done) {
        mocs_program(mocs_console);
    }

    rps_up(mocs_console);

    if ((error = engine_start(&RCS)) != GPU_OK) {
        return error;
    }

    if (runs_state == NULL && !gtt_pages(RUNS_PAGES, &runs_state, &runs_state_gtt)) {
        runs_state = NULL;
        return GPU_ERR_NO_MEMORY;
    }

    if ((run_given && (int32_t)(fence_reached(&RCS) - run_fence) < 0)
        || RCS.in_flight + 80u * q->nsteps + 64u > RING_ROOM) {
        (void)engine_idle(&RCS);
        return GPU_ERR_BUSY;
    }

    for (i = 0; i < RUNS_PAGES * 1024u; i++) {
        runs_state[i] = 0;
    }

    for (i = 0; i < q->nbuffers; i++) {
        (void)run_surface(runs_state + (RUN_SS_AT + i * 64u) / 4u, &B[q->buffers[i].id], q->buffers[i].view);
        runs_state[RUN_BT_AT / 4u + i] = RUN_SS_AT + i * 64u;
    }

    regs = q->nconstants / 32u;

    for (i = 0; i < q->nsteps; i++) {
        volatile uint32_t *curbe = runs_state + (RUNS_CURBE_AT + i * 256u) / 4u;
        volatile uint32_t *idd = runs_state + (RUNS_IDD_AT + i * 32u) / 4u;

        for (j = 0; j < q->nconstants / 4u; j++) {
            uint32_t w;

            __builtin_memcpy(&w, &q->constants[j * 4u], 4);
            curbe[j] = w;
        }

        curbe[q->step_at / 4u] = q->steps[i].step;

        idd[0] = 0;                                     /* the kernel, from the instruction base */
        idd[2] = 0;                                     /* IEEE, not single program flow (run) */
        idd[4] = RUN_BT_AT;
        idd[5] = regs << 16;
        idd[6] = 1u;
    }

    {
        const uint32_t sba_lo = runs_state_gtt | 1u;
        const uint32_t slot = RUN_TS_AT + 2u * ((RCS.issued + 1u) % RUN_TS_SLOTS);
        const uint32_t t0[4] = { IGPU_MI_STORE_REG_MEM, IGPU_RENDER_BASE + IGPU_RING_TIMESTAMP,
                                 RCS.status_gtt + slot * 4u, 0 };
        const uint32_t t1[4] = { IGPU_MI_STORE_REG_MEM, IGPU_RENDER_BASE + IGPU_RING_TIMESTAMP,
                                 RCS.status_gtt + (slot + 1u) * 4u, 0 };
        const uint32_t stall[6] = { IGPU_PIPE_CONTROL, IGPU_PC_CS_STALL, 0, 0, 0, 0 };

        for (i = 0; i < q->nsteps; i++) {
            const struct kernel *k = &K[q->steps[i].kernel];
            /* Before each step: the last one finished, its writes out of
             * the data port's caches, and the caches it reads made fresh. */
            const uint32_t before[6] = { IGPU_PIPE_CONTROL,
                                         IGPU_PC_CS_STALL | IGPU_PC_INVALIDATE | IGPU_PC_DC_FLUSH,
                                         0, 0, 0, 0 };
            const uint32_t select = IGPU_PIPELINE_SELECT;
            const uint32_t sba[19] = {
                IGPU_STATE_BASE_ADDRESS,
                1u, 0,
                1u,
                sba_lo, 0,
                sba_lo, 0,
                0, 0,
                k->gtt | 1u, 0,
                0xfffff000u | 1u, (RUNS_PAGES << 12) | 1u,
                0xfffff000u | 1u, (k->pages << 12) | 1u,
                1u, 0, 0xfffff000u,
            };
            const uint32_t vfe[9] = {
                IGPU_MEDIA_VFE_STATE, 0, 0,
                ((RUN_THREADS - 1u) << 16) | (1u << 8),
                0,
                regs > 0 ? regs : 1u,
                0, 0, 0,
            };
            const uint32_t curbe[4] = { IGPU_MEDIA_CURBE_LOAD, 0, q->nconstants, RUNS_CURBE_AT + i * 256u };
            const uint32_t idl[4] = { IGPU_MEDIA_IDL, 0, 32u, RUNS_IDD_AT + i * 32u };
            const uint32_t walk[15] = {
                IGPU_GPGPU_WALKER, 0, 0, 0,
                q->steps[i].simd == 16u ? 1u << 30 : 0u,
                0, 0, q->steps[i].groups_x,
                0, 0, q->steps[i].groups_y,
                0, 1u,
                q->steps[i].simd == 16u ? 0xffffu : 0xffu,
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

            emit(&RCS, idl, 4);

            if (i == 0) {
                emit(&RCS, t0, 4);
            }

            emit(&RCS, walk, 15);
            emit(&RCS, flush, 2);
        }

        emit(&RCS, stall, 6);
        emit(&RCS, t1, 4);
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
    unsigned long i, first, from = rq->first_page, count;

    if (!G.found) {
        return GPU_ERR_NO_DEVICE;
    }

    buffers_init();

    /* The region's pages from `first_page`, `pages` of them or the rest
     * (X7 G2): a model is imported a layer a buffer, each its own run of
     * the one region, rather than all of it once for every buffer. */
    if (cap < 0 || rq->x <= 0 || rq->y <= 0 || width > 16384u || height > 16384u
        || rq->pitch < width * 4u || rq->pitch >= 32768u || (pages = kosmos_mem_size(cap)) <= 0
        || from >= (unsigned long)pages) {
        return GPU_ERR_BAD_OP;
    }

    count = rq->pages != 0 ? rq->pages : (unsigned long)pages - from;

    if (count > (unsigned long)pages - from
        || (unsigned long)rq->offset + (unsigned long)rq->pitch * height > count * 4096ul) {
        return GPU_ERR_BAD_OP;
    }

    for (id = 1; id < BUFFERS_MAX && B[id].used; id++) {
    }

    if (id == BUFFERS_MAX) {
        return GPU_ERR_NO_MEMORY;
    }

    if (!igpu_slots_take(&slots, count, &first)) {
        return GPU_ERR_GTT_BUSY;
    }

    for (i = 0; i < count; i++) {
        long phys = kosmos_mem_page_phys(cap, from + i);

        if (phys < 0 || !gtt_write(first + i, (uint64_t)phys | 1u, NULL)) {
            while (i-- > 0) {
                (void)gtt_write(first + i, 0, NULL);
            }

            igpu_slots_give(&slots, first, count);
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
    B[id].pages = count;
    B[id].width = width;
    B[id].height = height;
    B[id].pitch = rq->pitch;

    rep->buffer.id = id;
    rep->buffer.gtt = B[id].gtt;
    rep->buffer.pitch = rq->pitch;
    rep->buffer.pages = (uint32_t)count;

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
    igpu_slots_give(&slots, B[id].slot, B[id].pages);   /* and used again (X7 G2) */
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
    unsigned long pages, i, slot;
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

    if (!igpu_slots_take(&slots, pages, &slot)) {
        return GPU_ERR_GTT_BUSY;
    }

    /* One run, because `kosmos_mem_phys` names only one; write-combining,
     * because the display does not look in the processor's caches. */
    region = kosmos_mem_create_flags(pages, MEM_CONTIGUOUS | MEM_WRITE_COMBINE);

    if (region < 0 || (phys = kosmos_mem_phys(region)) < 0) {
        if (region >= 0) {
            (void)kosmos_cap_drop(region);
        }

        igpu_slots_give(&slots, slot, pages);
        return GPU_ERR_NO_MEMORY;
    }

    for (i = 0; i < pages; i++) {
        if (!gtt_write(slot + i, (uint64_t)(phys + (long)(i * 4096u)) | 1u, NULL)) {
            (void)kosmos_cap_drop(region);
            igpu_slots_give(&slots, slot, pages);
            return GPU_ERR_GTT_BUSY;
        }
    }

    wreg(GFX_FLSH_CNTL, 1u);
    (void)reg(G.regs, GFX_FLSH_CNTL);

    B[id].used = true;
    B[id].kind = GPU_BUFFER_SCANOUT;
    B[id].slot = slot;
    flips_seen = true;
    B[id].holder = holder_of_request();
    B[id].cap = region;
    B[id].gtt = (uint32_t)(slot << 12);
    B[id].pages = pages;
    B[id].width = width;
    B[id].height = height;
    B[id].pitch = pitch;

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

/*--------------------------------------------------------------------------
 * **Programs' contexts** (`docs/gpu3d.md` §5.3, G6e): the 3D half's
 * verbs, kept by `intelgpu_ctx.c` and run by one of two backends - the
 * M700's engines on execlists, or, where there is no Intel GPU, the
 * stand-in that runs a batch's MI_ commands on the processor.
 *------------------------------------------------------------------------*/

/* GTT pages as `gtt_pages` gives them, with their region kept so that they
 * can be given back: a context's ring, image and fence go when it does. */
static bool gtt_alloc(unsigned long pages, volatile uint32_t **cpu, uint32_t *gtt, long *cap,
                      unsigned long flags)
{
    long region = kosmos_mem_create_flags(pages, MEM_CONTIGUOUS | flags);
    long at, phys;
    unsigned long i, first;

    buffers_init();
    *cap = region;

    if (region < 0) {
        return false;
    }

    if ((at = kosmos_mem_map(region)) < 0 || (phys = kosmos_mem_phys(region)) < 0
        || !igpu_slots_take(&slots, pages, &first)) {
        (void)kosmos_cap_drop(region);
        *cap = -1;
        return false;
    }

    for (i = 0; i < pages; i++) {
        (void)gtt_write(first + i, (uint64_t)(phys + (long)(i * 4096u)) | 1u, NULL);
    }

    wreg(GFX_FLSH_CNTL, 1u);
    (void)reg(G.regs, GFX_FLSH_CNTL);

    *cpu = (volatile uint32_t *)at;
    *gtt = (uint32_t)(first << 12);
    return true;
}

static void gtt_free(unsigned long pages, volatile uint32_t *cpu, uint32_t gtt, long cap)
{
    unsigned long i, first = gtt >> 12;

    if (cap < 0) {
        return;
    }

    for (i = 0; i < pages; i++) {
        (void)gtt_write(first + i, 0, NULL);
    }

    wreg(GFX_FLSH_CNTL, 1u);
    (void)reg(G.regs, GFX_FLSH_CNTL);
    igpu_slots_give(&slots, first, pages);
    kosmos_share_unmap((unsigned long)(uintptr_t)cpu, pages);
    (void)kosmos_cap_drop(cap);
}

/* A context's page-table pages: runs of PT_CHUNK, one physical run each,
 * write-combining when the GPU walks them. */
#define PT_CHUNK 16u

static void *pool_page(void *ctx, uint64_t *phys)
{
    struct ptpool *p = ctx;
    uint8_t *page;

    if (p->n == 0 || p->used == PT_CHUNK) {
        long region, at, base;

        if (p->n == p->room) {
            unsigned room = p->room == 0 ? 4u : p->room * 2u;
            long *caps = realloc(p->caps, room * sizeof *caps);
            uint8_t **cpu = caps != NULL ? realloc(p->cpu, room * sizeof *cpu) : NULL;
            uint64_t *ph = cpu != NULL ? realloc(p->phys, room * sizeof *ph) : NULL;

            if (caps != NULL) {
                p->caps = caps;
            }
            if (cpu != NULL) {
                p->cpu = cpu;
            }
            if (ph == NULL) {
                return NULL;
            }
            p->phys = ph;
            p->room = room;
        }

        region = kosmos_mem_create_flags(PT_CHUNK, MEM_CONTIGUOUS | (p->wc ? MEM_WRITE_COMBINE : 0u));
        if (region < 0) {
            return NULL;
        }
        if ((at = kosmos_mem_map(region)) < 0 || (base = kosmos_mem_phys(region)) < 0) {
            (void)kosmos_cap_drop(region);
            return NULL;
        }

        p->caps[p->n] = region;
        p->cpu[p->n] = (uint8_t *)at;
        p->phys[p->n] = (uint64_t)base;
        p->n++;
        p->used = 0;
    }

    page = p->cpu[p->n - 1u] + p->used * 4096u;
    *phys = p->phys[p->n - 1u] + p->used * 4096u;
    p->used++;
    memset(page, 0, 4096);
    return page;
}

static void *pool_cpu(void *ctx, uint64_t phys)
{
    struct ptpool *p = ctx;
    unsigned i;

    for (i = 0; i < p->n; i++) {
        if (phys >= p->phys[i] && phys < p->phys[i] + PT_CHUNK * 4096u) {
            return p->cpu[i] + (phys - p->phys[i]);
        }
    }

    return NULL;
}

static void pool_free(struct ptpool *p)
{
    unsigned i;

    for (i = 0; i < p->n; i++) {
        kosmos_share_unmap((unsigned long)(uintptr_t)p->cpu[i], PT_CHUNK);
        (void)kosmos_cap_drop(p->caps[i]);
    }

    free(p->caps);
    free(p->cpu);
    free(p->phys);
}

static struct engine *engine_of(const struct igpu_ctx *c)
{
    return c->engine == GPU_ENGINE_COPY ? &BCS : &RCS;
}

/* A context's page table and its fence, common to both backends: the fence
 * page in the global GTT when the GPU writes it, a plain page when the
 * stand-in does. */
static bool ctx_setup(struct igpu_ctx *c, bool gpu, long *fence_cap)
{
    struct hwctx *h = calloc(1, sizeof *h);
    struct igpu_pt_pages pages;

    if (h == NULL) {
        return false;
    }

    h->ring_cap = h->image_cap = h->fence_cap = h->fence_ro = -1;
    h->pool.wc = gpu;
    c->hw = h;
    pages.page = pool_page;
    pages.cpu = pool_cpu;
    pages.ctx = &h->pool;

    if (!igpu_ppgtt_init(&c->pt, &pages)) {
        return false;
    }

    if (gpu) {
        /* Write-back, not write-combining: a region the caller is handed
         * read-only cannot be the other (`SYS_MEM_READONLY`), and the GPU
         * writes it through the LLC the processor reads, as i915 keeps a
         * status page on a machine with one. */
        if (!gtt_alloc(1, &h->fence, &h->fence_gtt, &h->fence_cap, 0)) {
            return false;
        }
    } else {
        long at;

        if ((h->fence_cap = kosmos_mem_create(1)) < 0 || (at = kosmos_mem_map(h->fence_cap)) < 0) {
            return false;
        }
        h->fence = (volatile uint32_t *)at;
    }

    h->fence[0] = h->fence[1] = 0;
    c->fence = (volatile uint64_t *)h->fence;

    if ((h->fence_ro = kosmos_mem_readonly(h->fence_cap)) < 0) {
        return false;
    }

    *fence_cap = h->fence_ro;
    return true;
}

static void ctx_teardown(struct igpu_ctx *c, bool gpu)
{
    struct hwctx *h = c->hw;

    if (h == NULL) {
        return;
    }

    if (gpu) {
        struct engine *e = engine_of(c);
        uint32_t id = (uint32_t)(c - CTXS.list) + 1u;
        unsigned i;

        /* Its image is the engine's until the engine says it went idle,
         * which follows the fence within microseconds. */
        for (i = 0; i < 250 && e->active && e->running == id; i++) {
            el_poll(e);
            if (e->active && e->running == id) {
                kosmos_sleep(1);
            }
        }
        if (e->running == id) {
            e->running = 0;
        }

        gtt_free(RING_PAGES, h->ring, h->ring_gtt, h->ring_cap);
        gtt_free(e->render ? IGPU_LRC_PAGES_RENDER : IGPU_LRC_PAGES_OTHER, h->image, h->image_gtt,
                 h->image_cap);
        gtt_free(1, h->fence, h->fence_gtt, h->fence_cap);
    } else if (h->fence_cap >= 0) {
        kosmos_share_unmap((unsigned long)(uintptr_t)h->fence, 1);
        (void)kosmos_cap_drop(h->fence_cap);
    }

    if (h->fence_ro >= 0) {
        (void)kosmos_cap_drop(h->fence_ro);
    }

    pool_free(&h->pool);
    free(h);
    c->hw = NULL;
}

/*---- the M700's engines ----*/

static bool hw_setup(void *env, struct igpu_ctx *c, long *fence_cap)
{
    struct engine *e = engine_of(c);
    struct hwctx *h;

    (void)env;

    if (engine_start(e) != GPU_OK || !ctx_setup(c, true, fence_cap)) {
        ctx_teardown(c, true);
        return false;
    }

    h = c->hw;

    if (!gtt_alloc(RING_PAGES, &h->ring, &h->ring_gtt, &h->ring_cap, MEM_WRITE_COMBINE)
        || !gtt_alloc(e->render ? IGPU_LRC_PAGES_RENDER : IGPU_LRC_PAGES_OTHER, &h->image, &h->image_gtt,
                      &h->image_cap, MEM_WRITE_COMBINE)) {
        ctx_teardown(c, true);
        return false;
    }

    return true;
}

static void hw_teardown(void *env, struct igpu_ctx *c)
{
    (void)env;
    ctx_teardown(c, true);
}

/* Commands into a program's ring, wrapping as `ring_put` does. */
static void hw_put(struct hwctx *h, const uint32_t *dw, uint32_t n)
{
    uint32_t i;

    if (h->tail + n > RING_DWORDS) {
        h->in_flight += RING_DWORDS - h->tail;
        while (h->tail < RING_DWORDS) {
            h->ring[h->tail++] = IGPU_MI_NOOP;
        }
        h->tail = 0;
    }

    h->in_flight += n;
    for (i = 0; i < n; i++) {
        h->ring[h->tail++] = dw[i];
    }
    h->tail %= RING_DWORDS;
}

/*
 * One exec into the context's ring, as i915 builds a request: the TLB and
 * the read caches invalidated (`gen8_emit_flush_rcs` with EMIT_INVALIDATE,
 * or the blitter's MI_FLUSH_DW), the program's batch started from its own
 * page table - bit 8, so the engine runs it unprivileged - then the caches
 * flushed and the timeline's value written to the fence page as a qword
 * (`gen8_emit_fini_breadcrumb`). False while the ring has no room: the
 * work is offered again from the driver's loop.
 */
#define IGPU_MI_BB_PPGTT     (1u << 8)
#define IGPU_MI_FLUSH_DW_QW  (((0x26u << 23) | 3u) | (1u << 14))   /* five dwords: a qword stored */

static bool hw_give(void *env, struct igpu_ctx *c, const struct igpu_exec *x)
{
    struct engine *e = engine_of(c);
    struct hwctx *h = c->hw;
    uint32_t dw[40], n = 0;

    (void)env;

    if (igpu_ctx_reached(c) >= h->given) {
        h->in_flight = 0;
    }

    if (h->in_flight + 64u > RING_ROOM) {
        return false;
    }

    if (e->render) {
        dw[n++] = IGPU_PIPE_CONTROL;
        dw[n++] = IGPU_PC_CS_STALL | IGPU_PC_TLB_INVALIDATE | IGPU_PC_INVALIDATE | IGPU_PC_VF_INVALIDATE
                | IGPU_PC_QW_WRITE | IGPU_PC_GLOBAL_GTT;
        dw[n++] = h->fence_gtt + 64u;                       /* a scratch qword past the fence */
        dw[n++] = 0;
        dw[n++] = 0;
        dw[n++] = 0;
    } else {
        dw[n++] = IGPU_MI_FLUSH_DW_STORE | IGPU_MI_INVALIDATE_TLB;
        dw[n++] = (h->fence_gtt + 64u) | IGPU_MI_FLUSH_DW_GTT;
        dw[n++] = 0;
        dw[n++] = 0;
    }

    dw[n++] = IGPU_MI_BB_START | IGPU_MI_BB_PPGTT;
    dw[n++] = (uint32_t)x->batch;
    dw[n++] = (uint32_t)(x->batch >> 32) & 0xffffu;

    if (e->render) {
        dw[n++] = IGPU_PIPE_CONTROL;
        dw[n++] = IGPU_PC_CS_STALL | IGPU_PC_TLB_INVALIDATE | IGPU_PC_RT_FLUSH | IGPU_PC_DEPTH_FLUSH
                | IGPU_PC_DC_FLUSH;
        dw[n++] = 0;
        dw[n++] = 0;
        dw[n++] = 0;
        dw[n++] = 0;
        dw[n++] = IGPU_PIPE_CONTROL;
        dw[n++] = IGPU_PC_FLUSH_ENABLE | IGPU_PC_CS_STALL | IGPU_PC_QW_WRITE | IGPU_PC_GLOBAL_GTT;
        dw[n++] = h->fence_gtt;
        dw[n++] = 0;
        dw[n++] = (uint32_t)x->signal;
        dw[n++] = (uint32_t)(x->signal >> 32);
    } else {
        dw[n++] = IGPU_MI_FLUSH_DW_QW;
        dw[n++] = h->fence_gtt | IGPU_MI_FLUSH_DW_GTT;
        dw[n++] = 0;
        dw[n++] = (uint32_t)x->signal;
        dw[n++] = (uint32_t)(x->signal >> 32);
    }

    if ((n & 1u) != 0) {
        dw[n++] = IGPU_MI_NOOP;                              /* the tail is in qwords */
    }

    hw_put(h, dw, n);
    h->given = x->signal;

#if defined(__x86_64__)
    __asm__ volatile("sfence" ::: "memory");
#endif

    (void)forcewake(true);
    el_poll(e);                                              /* sent now if the engine is idle */
    return true;
}

static long ctx_page_phys(long cap, unsigned long index)
{
    return kosmos_mem_page_phys(cap, index);
}

static long ctx_region_pages(long cap)
{
    return kosmos_mem_size(cap);
}

static long ctx_drop(long cap)
{
    return kosmos_cap_drop(cap);
}

static const struct igpu_ctx_backend HW_BACKEND = {
    NULL, hw_setup, hw_teardown, hw_give, NULL, NULL,
    ctx_page_phys, ctx_region_pages, kosmos_pulse, ctx_drop,
};

/*---- the stand-in ----*/

static bool standin_setup(void *env, struct igpu_ctx *c, long *fence_cap)
{
    (void)env;

    if (!ctx_setup(c, false, fence_cap)) {
        ctx_teardown(c, false);
        return false;
    }

    return true;
}

static void standin_teardown(void *env, struct igpu_ctx *c)
{
    (void)env;
    ctx_teardown(c, false);
}

/* Run at once, on the processor: the engine is never busy, and the value
 * written as reached whatever the batch did - a fault or a hang is the
 * context's to report, as a reset's would be. */
static bool standin_give(void *env, struct igpu_ctx *c, const struct igpu_exec *x)
{
    struct hwctx *h = c->hw;

    (void)env;
    (void)igpu_standin_run(c, x->batch, x->length);
    h->fence[1] = (uint32_t)(x->signal >> 32);
    h->fence[0] = (uint32_t)x->signal;
    return true;
}

static uint8_t *standin_map(void *env, long cap)
{
    long at = kosmos_mem_map(cap);

    (void)env;
    return at < 0 ? NULL : (uint8_t *)at;
}

static void standin_unmap(void *env, uint8_t *cpu, long cap)
{
    long pages = kosmos_mem_size(cap);

    (void)env;
    if (pages > 0) {
        kosmos_share_unmap((unsigned long)(uintptr_t)cpu, (unsigned long)pages);
    }
}

static const struct igpu_ctx_backend STANDIN_BACKEND = {
    NULL, standin_setup, standin_teardown, standin_give, standin_map, standin_unmap,
    ctx_page_phys, ctx_region_pages, kosmos_pulse, ctx_drop,
};

/*---- the verbs ----*/

static bool ctx_busy;                       /* work or waiters outstanding, from the last poll */

static uint32_t ctx_request(long console, const struct gpu_ctx_request *q, long *cap,
                            struct gpu_reply *rep, long *pass)
{
    const uint32_t who = holder_of_request();
    uint32_t error = GPU_OK, id;
    struct igpu_ctx *c;
    struct say_line line;

    if (CTXS.be == NULL) {
        CTXS.be = G.found ? &HW_BACKEND : &STANDIN_BACKEND;
        say_begin(&line);
        say_text(&line, G.found ? "intelgpu: programs' contexts on the GPU's engines, on execlists"
                                : "intelgpu: programs' contexts on the stand-in: no Intel GPU, so a batch's"
                                  " MI_ commands are run by the processor");
        say_send(console, &line);
    }

    switch (q->op) {
    case GPU_OP_CONTEXT_NEW: {
        long fence = -1;

        error = igpu_ctx_new(&CTXS, who, q->engine, &id, &fence);
        if (error == GPU_OK) {
            rep->words[0] = id;
            *pass = fence;                  /* a copy: the context keeps its own */
        }
        break;
    }
    case GPU_OP_CONTEXT_FORGET:
        error = igpu_ctx_forget(&CTXS, q->context, who);
        break;
    case GPU_OP_BIND:
        error = igpu_ctx_bind(&CTXS, q->context, who, q->gpu, *cap, q->first_page, q->pages, q->flags);
        if (error == GPU_OK) {
            *cap = -1;                      /* the context's now */
        }
        break;
    case GPU_OP_UNBIND:
        error = igpu_ctx_unbind(&CTXS, q->context, who, q->gpu);
        break;
    case GPU_OP_EXEC: {
        struct igpu_exec x;
        uint32_t i;

        memset(&x, 0, sizeof x);
        x.batch = q->gpu;
        x.length = q->pages;
        x.signal = q->signal;
        x.nwaits = q->flags;

        if (x.nwaits > GPU_EXEC_WAITS) {
            error = GPU_ERR_BAD_OP;
            break;
        }
        for (i = 0; i < x.nwaits; i++) {
            x.waits[i].context = q->waits[i].context;
            x.waits[i].value = q->waits[i].value;
        }

        error = igpu_ctx_exec(&CTXS, q->context, who, &x);
        if (error == GPU_OK) {
            rep->words[0] = (uint32_t)x.signal;
            rep->words[1] = (uint32_t)(x.signal >> 32);
        }
        break;
    }
    case GPU_OP_FENCE_WAIT:
        error = igpu_ctx_fence_wait(&CTXS, q->context, who, q->signal, *cap, q->flags);
        if (error == GPU_OK) {
            *cap = -1;                      /* the context's, until it is pulsed */
        }
        break;
    case GPU_OP_CONTEXT_STATE:
        if ((c = igpu_ctx_get(&CTXS, q->context, who, &error)) != NULL) {
            uint64_t reached = igpu_ctx_reached(c);

            rep->words[0] = (uint32_t)reached;
            rep->words[1] = (uint32_t)(reached >> 32);
            rep->words[2] = (uint32_t)c->queued;
            rep->words[3] = (uint32_t)(c->queued >> 32);
            rep->words[4] = c->hangs;
            rep->words[5] = c->faults;
            rep->words[6] = c->fault_word;
        }
        break;
    default:
        error = GPU_ERR_BAD_OP;
        break;
    }

    ctx_busy = igpu_ctxs_poll(&CTXS);
    return error;
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

        /* Back within a tick while an engine is working or holds work it
         * was handed as it ran, so that work goes when the engine goes idle
         * and a hang is seen, even if nobody asks for anything (G6d). */
        {
            const bool busy = el_waiting(&RCS) || el_waiting(&BCS) || RCS.active || BCS.active
                              || ctx_busy;
            long got;

            if (irq_armed) {
                /* The line and the endpoint at once; the tick still, while
                 * work is out, so a hang is seen when nothing interrupts. */
                const long lines[1] = { gpu_irq };
                const long ends[1] = { endpoint };

                if (kosmos_irq_wait_any(lines, 1, busy ? 1u : 0u, ends, 1) == 0) {
                    irq_service();
                }
                got = kosmos_receive(endpoint, &msg, &sender, 1, 0);
            } else {
                got = kosmos_receive(endpoint, &msg, &sender, 0, busy ? 1u : 0u);
            }

            el_poll(&RCS);
            el_poll(&BCS);

            /* Programs' contexts: pulses for values reached, and work
             * whose waits are met given to its engine (G6e). */
            if (CTXS.be != NULL) {
                ctx_busy = igpu_ctxs_poll(&CTXS);
            }

            if (got != 0) {
                continue;
            }
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
            case GPU_OP_HANG_TEST:
                hang_test(console, rq.x == 1 ? &RCS : &BCS, &rep);
                break;
            case GPU_OP_EXECLIST_TEST:
                /* `x`: 1 or 2 contexts, or 3 for two sharing one page table */
                execlist_test(console, rq.x == 0 ? 1u : rq.x == 3 ? 2u : (unsigned)rq.x, rq.x == 3, &rep);
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
            case GPU_OP_RUNS: {
                static struct gpu_runs qs;

                memset(&qs, 0, sizeof qs);
                memcpy(&qs, msg.data, msg.length < sizeof qs ? msg.length : sizeof qs);
                rep.error = msg.length == sizeof qs ? runs(&qs, &rep) : GPU_ERR_BAD_OP;
                break;
            }
            case GPU_OP_RUN: {
                static struct gpu_run q;

                memset(&q, 0, sizeof q);
                memcpy(&q, msg.data, msg.length < sizeof q ? msg.length : sizeof q);
                rep.error = msg.length == sizeof q ? run(&q, &rep) : GPU_ERR_BAD_OP;
                break;
            }
            case GPU_OP_CONTEXT_NEW:
            case GPU_OP_CONTEXT_FORGET:
            case GPU_OP_BIND:
            case GPU_OP_UNBIND:
            case GPU_OP_EXEC:
            case GPU_OP_FENCE_WAIT:
            case GPU_OP_CONTEXT_STATE: {
                static struct gpu_ctx_request q;

                memset(&q, 0, sizeof q);
                memcpy(&q, msg.data, msg.length < sizeof q ? msg.length : sizeof q);
                rep.error = msg.length == sizeof q ? ctx_request(console, &q, &cap, &rep, &pass)
                                                   : GPU_ERR_BAD_OP;
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
