/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Mac Pro 2013's GPUs, driven** (`docs/amdgpu.md`): a pair of AMD
 * FirePro D300, D500 or D700 - Southern Islands, GCN 1.0 - found, named,
 * looked at, and their DMA engines and the display's page flip tried.
 *
 *   find     the board says where each card's BARs are     SYS_DEV_FIND
 *            and what its configuration space says         SYS_DEV_CONFIG
 *   map      BAR 2's first 76 KB, the registers, uncached   SYS_DEV_MAP
 *   read     which chip, its memory and where the memory controller puts
 *            it, its compute units, its engines' state, and each of the
 *            six display controllers: what it scans out, from where
 *   say      all of it, a line a thing
 *   try      each DMA engine's first work - a number written, 64 KB
 *            filled and copied, a fence - when the memory controller is
 *            known to take an address untranslated; and, asked for, a page
 *            flip to a picture of its own and back (`gpu cursor 5`)
 *
 * Then it answers `/Devices/gpu` in `gpuproto.h`'s shape, as `intelgpu.c`
 * does on the M700: GPU_OP_LOOK, GPU_OP_STATS, GPU_OP_ENGINE_TEST (`x` 0
 * the first DMA engine, 1 the second, on every card that can) and
 * GPU_OP_CURSOR_TEST, which on this GPU is the flip test - a test picture
 * on the screen for `seconds`, put there by the display controller rather
 * than by a cursor plane. Everything else is GPU_ERR_NO_DEVICE, which is
 * what the window manager hears on a machine without the M700's GPU and
 * what makes it draw its own arrow and compose into the firmware's screen.
 *
 * **On any surprise it says so and leaves the screen alone**: nothing at
 * boot writes the display controller, and the DMA test runs only where the
 * memory controller's own registers say a VRAM address reaches VRAM.
 *
 * Nothing here waits on hardware for long: every wait is a poll of a
 * register or a word, a scheduler tick apart, given up after a bound.
 *
 * No address of its own: the BARs are `hal/pc/devices.c`'s; every register
 * offset is `amdgpu_decode.h`'s, from Linux's radeon and amdgpu.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "gpuproto.h"
#include "amdgpu_decode.h"
#include "mmio.h"
#include "init/say.h"

/*
 * The device kinds. **Placeholders**, as `tg3.c`'s is: the kernel knows
 * DEV_INTEL_GPU 6 and the Broadcom lane holds 7; the main session adds
 * HAL_DEV_ and DEV_AMD_GPU (BAR 2, the registers) and DEV_AMD_VRAM (BAR 0,
 * the aperture) with the next free numbers, a vendor 0x1002 class 03 match
 * in `hal/pc/devices.c`, and `hal_device_config` for the first
 * (`docs/amdgpu.md`, *What the kernel needs*). Until then `kosmos_dev_find`
 * answers no for both and this driver says nothing and returns.
 */
#ifndef DEV_AMD_GPU
#define DEV_AMD_GPU             8u
#endif
#ifndef DEV_AMD_VRAM
#define DEV_AMD_VRAM            9u
#endif

#define CARDS_MAX               2u      /* the Mac Pro has two; a third is told about nothing */

#define PCI_COMMAND_WORD        0x04u   /* command 15:0, status 31:16 */
#define PCI_CLASS_WORD          0x08u   /* revision 7:0, class 31:8 */
#define PCI_SUBSYSTEM_WORD      0x2cu   /* subsystem vendor 15:0, id 31:16 */

/*
 * **Where in VRAM this driver works**, as offsets into BAR 0, past the
 * firmware's screen on the card that shows it and from 16 MB on the other:
 * the two DMA rings, a page of words they write, and 256 KB the fill and
 * copy test uses - and, after that, the flip test's picture.
 */
#define WORK_RING0              0x00000u
#define WORK_RING1              0x01000u
#define WORK_WORDS              0x02000u    /* engine e's words at e * 0x100 */
#define WORK_TEST               0x10000u    /* engine e: source at + e * 0x20000, its copy 64 KB on */
#define WORK_BYTES              0x50000u
#define RING_BYTES              0x1000u
#define TEST_BYTES              0x10000u
#define TEST_PATTERN            0x00c0ffeeu
#define WRITE_VALUE             0xdeadbeefu
#define WAIT_TICKS              50u         /* a DMA test's longest wait: 200 ms at 250 a second */

struct engine_result {
    uint32_t error;
    uint32_t value, fence, rptr, wptr, rb_cntl, status, srbm2;
    uint32_t filled, copied;            /* of TEST_BYTES / 4 words */
    uint32_t elapsed_us;
};

struct card {
    bool            found;
    bool            answers;            /* its registers read as something */
    bool            posted;             /* its memory controller runs */
    struct dev_info regs_dev, vram_dev;
    bool            have_vram;
    uintptr_t       regs;
    uint16_t        device;
    uint32_t        subsystem, command, class_word;
    enum amdgpu_family family;
    const char     *name;
    uint32_t        vram_mb;
    unsigned        vram_bits;
    uint64_t        fb_start, fb_end;
    uint32_t        hdp_base;
    uint64_t        aperture;           /* BAR 0's bytes, 0 when not known */
    bool            untranslated;       /* a VRAM address reaches VRAM */
    uint32_t        vm_tlb, vm_ctx0;    /* MC_VM_MX_L1_TLB_CNTL, VM_CONTEXT0_CNTL */
    uint64_t        sys_low, sys_high;  /* the system aperture, inclusive */
    bool            sys_set;
    int             crtc;               /* the lit controller, or -1 */
    struct amdgpu_grph grph;
    uint64_t        fb_offset;          /* the screen's offset in BAR 0 */
    bool            fb_in_bar;
    uint64_t        work;               /* WORK_* from here, in BAR 0 */
    bool            work_ok;
    uintptr_t       work_at;            /* mapped, or 0 */
    unsigned        cus, cus_max;
    uint32_t        flips;
};

static struct card CARD[CARDS_MAX];
static unsigned ncards;
static struct sysinfo machine;
static bool machine_known;

static uint32_t rd(const struct card *c, uint32_t offset)
{
    return mmio_read32(c->regs + offset);
}

static void wr(const struct card *c, uint32_t offset, uint32_t value)
{
    mmio_write32(c->regs + offset, value);
}

/* A word of the work area, through BAR 0. */
static uint32_t vram_rd(const struct card *c, uint32_t offset)
{
    return mmio_read32(c->work_at + offset);
}

static void vram_wr(const struct card *c, uint32_t offset, uint32_t value)
{
    mmio_write32(c->work_at + offset, value);
}

/*
 * The host data path, both ways (`si.c`'s `si_flush_hdp` and
 * `si_invalidate_hdp` in amdgpu): written 1 and read back, so what the
 * processor wrote through BAR 0 has reached VRAM before an engine reads it,
 * and what an engine wrote is not answered from the HDP's cache.
 */
static void hdp_flush(const struct card *c)
{
    wr(c, AMDGPU_HDP_MEM_FLUSH, 1);
    (void)rd(c, AMDGPU_HDP_MEM_FLUSH);
}

static void hdp_invalidate(const struct card *c)
{
    wr(c, AMDGPU_HDP_DEBUG0, 1);
    (void)rd(c, AMDGPU_HDP_DEBUG0);
}

static void begin(struct say_line *line, unsigned i)
{
    say_begin(line);
    say_text(line, "amdgpu: card ");
    say_dec(line, i);
    say_text(line, ": ");
}

static void say_mb(struct say_line *line, uint64_t bytes)
{
    say_dec(line, (unsigned long)(bytes >> 20));
    say_text(line, " MB");
}

static unsigned long counter_us(unsigned long t0, unsigned long t1)
{
    if (!machine_known || machine.counter_hz < 1000000u || t1 < t0) {
        return 0;
    }

    return (t1 - t0) / (unsigned long)(machine.counter_hz / 1000000u);
}

/*--------------------------------------------------------------------------
 * Found and named.
 *------------------------------------------------------------------------*/

static long config(unsigned i, unsigned offset)
{
    return kosmos_dev_config(DEV_AMD_GPU, i, offset);
}

static bool find(long console, unsigned i, struct card *c)
{
    struct say_line line;
    const struct amdgpu_family_info *f;
    long word, mapped;

    memset(c, 0, sizeof *c);
    c->crtc = -1;

    if (kosmos_dev_find(DEV_AMD_GPU, i, &c->regs_dev) != 0) {
        return false;
    }

    c->found = true;
    c->have_vram = kosmos_dev_find(DEV_AMD_VRAM, i, &c->vram_dev) == 0;
    c->device = (uint16_t)(c->regs_dev.id & 0xffffu);
    c->family = amdgpu_family_of(c->device);
    f = amdgpu_family_info(c->family);

    word = config(i, PCI_SUBSYSTEM_WORD);
    c->subsystem = word >= 0 ? (uint32_t)word : 0u;
    word = config(i, PCI_CLASS_WORD);
    c->class_word = word >= 0 ? (uint32_t)word : 0u;
    word = config(i, PCI_COMMAND_WORD);
    c->command = word >= 0 ? (uint32_t)word : 0u;
    c->name = amdgpu_card_name(c->device, c->subsystem);

    begin(&line, i);
    say_hex(&line, (c->regs_dev.where >> 8) & 0xffu, 2);
    say_text(&line, ":");
    say_hex(&line, (c->regs_dev.where >> 3) & 0x1fu, 2);
    say_text(&line, ".");
    say_dec(&line, c->regs_dev.where & 0x7u);
    say_text(&line, " 1002:");
    say_hex(&line, c->device, 4);
    say_text(&line, " rev ");
    say_hex(&line, c->class_word & 0xffu, 2);
    say_text(&line, ", subsystem ");
    say_hex(&line, c->subsystem & 0xffffu, 4);
    say_text(&line, ":");
    say_hex(&line, c->subsystem >> 16, 4);
    say_text(&line, ", class ");
    say_hex(&line, c->class_word >> 8, 6);
    say_text(&line, " - ");
    say_text(&line, c->name != NULL ? c->name : "a card this table does not name");
    say_text(&line, ", ");
    say_text(&line, f->name);
    say_send(console, &line);

    begin(&line, i);
    say_text(&line, "registers (BAR 2) at ");
    say_hex(&line, (unsigned long)c->regs_dev.base, 10);
    say_text(&line, ", VRAM aperture (BAR 0) ");

    if (c->have_vram) {
        say_text(&line, "at ");
        say_hex(&line, (unsigned long)c->vram_dev.base, 10);
    } else {
        say_text(&line, "not told");
    }

    say_text(&line, "; command ");
    say_hex(&line, c->command & 0xffffu, 4);
    say_text(&line, (c->command & 0x2u) ? " (memory on" : " (MEMORY OFF");
    say_text(&line, (c->command & 0x4u) ? ", bus master on)" : ", bus master off)");
    say_send(console, &line);

    if (c->family == AMDGPU_NOT_SI) {
        begin(&line, i);
        say_text(&line, "not a Southern Islands chip - looked at no further");
        say_send(console, &line);
        return true;
    }

    mapped = kosmos_dev_map((unsigned long)c->regs_dev.base, AMDGPU_REGS_BYTES / 4096u);

    if (mapped < 0) {
        begin(&line, i);
        say_text(&line, "its registers could not be mapped: ");
        say_dec(&line, (unsigned long)-mapped);
        say_send(console, &line);
        return true;
    }

    c->regs = (uintptr_t)mapped;
    return true;
}

/*--------------------------------------------------------------------------
 * Looked at: nothing written but GRBM_GFX_INDEX, put back as Linux leaves it.
 *------------------------------------------------------------------------*/

static void look_chip(long console, unsigned i, struct card *c)
{
    struct say_line line;
    const struct amdgpu_family_info *f = amdgpu_family_info(c->family);
    uint32_t straps = rd(c, AMDGPU_CC_DRM_ID_STRAPS);
    uint32_t memsize, aper, ramcfg, chmap, misc0, sup, fbloc, lo, hi, tlb, l2, ctx0;
    unsigned se, sh;

    c->answers = straps != 0xffffffffu;

    begin(&line, i);
    say_text(&line, "CC_DRM_ID_STRAPS ");
    say_hex(&line, straps, 8);

    if (!c->answers) {
        say_text(&line, " - every bit one: the registers do not answer; nothing more");
        say_send(console, &line);
        return;
    }

    say_text(&line, ((straps >> 4) & 0xffffu) == c->device
                    ? " - the device id in 19:4 matches PCI's: these are its registers"
                    : " - the device id in 19:4 DIFFERS from PCI's");
    say_send(console, &line);

    memsize = rd(c, AMDGPU_CONFIG_MEMSIZE);
    aper = rd(c, AMDGPU_CONFIG_APER_SIZE);
    ramcfg = rd(c, AMDGPU_MC_ARB_RAMCFG);
    chmap = rd(c, AMDGPU_MC_SHARED_CHMAP);
    misc0 = rd(c, AMDGPU_MC_SEQ_MISC0);
    sup = rd(c, AMDGPU_MC_SEQ_SUP_CNTL);
    fbloc = rd(c, AMDGPU_MC_VM_FB_LOCATION);
    c->hdp_base = rd(c, AMDGPU_HDP_NONSURFACE_BASE);
    lo = rd(c, AMDGPU_MC_VM_SYS_APER_LOW);
    hi = rd(c, AMDGPU_MC_VM_SYS_APER_HIGH);
    tlb = rd(c, AMDGPU_MC_VM_MX_L1_TLB_CNTL);
    l2 = rd(c, AMDGPU_VM_L2_CNTL);
    ctx0 = rd(c, AMDGPU_VM_CONTEXT0_CNTL);

    c->vram_mb = amdgpu_vram_mb(memsize);
    c->vram_bits = amdgpu_vram_bits(ramcfg, chmap);
    c->posted = (sup & 1u) != 0 && c->vram_mb != 0;
    amdgpu_fb_location(fbloc, &c->fb_start, &c->fb_end);

    /* BAR 0's size: the board's when it sized it, else CONFIG_APER_SIZE,
     * believed only as a power of two between 16 MB and 16 GB. */
    if (c->have_vram && c->vram_dev.size != 0) {
        c->aperture = c->vram_dev.size;
    } else if (aper >= (16u << 20) && (aper & (aper - 1u)) == 0) {
        c->aperture = aper;
    }

    begin(&line, i);
    say_dec(&line, c->vram_mb);
    say_text(&line, " MB of VRAM (CONFIG_MEMSIZE ");
    say_hex(&line, memsize, 8);
    say_text(&line, "), ");
    say_dec(&line, c->vram_bits);
    say_text(&line, "-bit, ");
    say_text(&line, ((misc0 >> 28) & 0xfu) == 5u ? "GDDR5" : "not GDDR5 by MC_SEQ_MISC0");
    say_text(&line, " vendor ");
    say_dec(&line, (misc0 >> 8) & 0xfu);
    say_text(&line, "; memory controller ");
    say_text(&line, (sup & 1u) ? "running" : "NOT RUNNING - the firmware did not post this card");
    say_send(console, &line);

    begin(&line, i);
    say_text(&line, "VRAM at MC ");
    say_hex(&line, (unsigned long)c->fb_start, 10);
    say_text(&line, "-");
    say_hex(&line, (unsigned long)c->fb_end, 10);
    say_text(&line, ", BAR 0 begins at MC ");
    say_hex(&line, (unsigned long)c->hdp_base << 8, 10);
    say_text(&line, ", aperture ");

    if (c->aperture != 0) {
        say_mb(&line, c->aperture);
    } else {
        say_text(&line, "unknown");
    }

    say_text(&line, " (CONFIG_APER_SIZE ");
    say_hex(&line, aper, 8);
    say_text(&line, ")");
    say_send(console, &line);

    c->vm_tlb = tlb;
    c->vm_ctx0 = ctx0;
    c->sys_low = (uint64_t)lo << 12;
    c->sys_high = ((uint64_t)hi << 12) | 0xfffu;
    c->sys_set = hi != 0;

    begin(&line, i);
    say_text(&line, "VM: L1 TLB ");
    say_hex(&line, tlb, 8);
    say_text(&line, " (system access mode ");
    say_dec(&line, (tlb >> 3) & 3u);
    say_text(&line, "), L2 ");
    say_hex(&line, l2, 8);
    say_text(&line, ", context 0 ");
    say_hex(&line, ctx0, 8);
    say_text(&line, ", system aperture ");
    say_hex(&line, (unsigned long)c->sys_low, 10);
    say_text(&line, "-");
    say_hex(&line, (unsigned long)c->sys_high, 10);
    say_send(console, &line);

    /* Compute units: each shader array selected, read, and broadcast again. */
    c->cus = 0;
    c->cus_max = f->shader_engines * f->sh_per_se * f->cu_per_sh;

    for (se = 0; se < f->shader_engines; se++) {
        for (sh = 0; sh < f->sh_per_se; sh++) {
            wr(c, AMDGPU_GRBM_GFX_INDEX, (1u << 30) | (se << 16) | (sh << 8));
            c->cus += amdgpu_cus_active(rd(c, AMDGPU_CC_GC_SHADER_ARRAY),
                                        rd(c, AMDGPU_GC_USER_SHADER_ARRAY), f->cu_per_sh);
        }
    }

    wr(c, AMDGPU_GRBM_GFX_INDEX, AMDGPU_GFX_BROADCAST);

    begin(&line, i);
    say_dec(&line, c->cus);
    say_text(&line, " of ");
    say_dec(&line, c->cus_max);
    say_text(&line, " compute units active (");
    say_dec(&line, c->cus * 64u);
    say_text(&line, " stream processors), ");
    say_dec(&line, amdgpu_temperature(rd(c, AMDGPU_CG_MULT_THERMAL)));
    say_text(&line, " C");
    say_send(console, &line);

    begin(&line, i);
    say_text(&line, "GRBM ");
    say_hex(&line, rd(c, AMDGPU_GRBM_STATUS), 8);
    say_text(&line, ", SRBM ");
    say_hex(&line, rd(c, AMDGPU_SRBM_STATUS), 8);
    say_text(&line, " ");
    say_hex(&line, rd(c, AMDGPU_SRBM_STATUS2), 8);
    say_text(&line, ", soft reset ");
    say_hex(&line, rd(c, AMDGPU_SRBM_SOFT_RESET), 8);
    say_text(&line, ", CP_ME_CNTL ");
    say_hex(&line, rd(c, AMDGPU_CP_ME_CNTL), 8);
    say_text(&line, ", RLC ");
    say_hex(&line, rd(c, AMDGPU_RLC_CNTL), 8);
    say_send(console, &line);

    for (se = 0; se < AMDGPU_DMA_ENGINES; se++) {
        uint32_t at = se ? AMDGPU_DMA1_OFFSET : 0u;

        begin(&line, i);
        say_text(&line, "DMA ");
        say_dec(&line, se);
        say_text(&line, ": RB_CNTL ");
        say_hex(&line, rd(c, AMDGPU_DMA_RB_CNTL + at), 8);
        say_text(&line, ", RB_BASE ");
        say_hex(&line, rd(c, AMDGPU_DMA_RB_BASE + at), 8);
        say_text(&line, ", rptr ");
        say_hex(&line, rd(c, AMDGPU_DMA_RB_RPTR + at), 5);
        say_text(&line, ", wptr ");
        say_hex(&line, rd(c, AMDGPU_DMA_RB_WPTR + at), 5);
        say_text(&line, ", status ");
        say_hex(&line, rd(c, AMDGPU_DMA_STATUS + at), 8);
        say_text(&line, ", power gating ");
        say_hex(&line, rd(c, AMDGPU_DMA_PG + at), 8);
        say_send(console, &line);
    }
}

static void look_display(long console, unsigned i, struct card *c)
{
    struct say_line line;
    const struct amdgpu_family_info *f = amdgpu_family_info(c->family);
    unsigned k, lit = 0;

    begin(&line, i);
    say_text(&line, "VGA: render ");
    say_hex(&line, rd(c, AMDGPU_VGA_RENDER_CONTROL), 8);
    say_text(&line, ", D1 ");
    say_hex(&line, rd(c, AMDGPU_D1VGA_CONTROL), 8);
    say_text(&line, ", D2 ");
    say_hex(&line, rd(c, AMDGPU_D2VGA_CONTROL), 8);
    say_send(console, &line);

    c->crtc = -1;

    for (k = 0; k < f->crtcs; k++) {
        uint32_t o = amdgpu_crtc_offset(k);
        struct amdgpu_grph_regs r = {
            rd(c, AMDGPU_CRTC_CONTROL + o), rd(c, AMDGPU_GRPH_ENABLE + o),
            rd(c, AMDGPU_GRPH_CONTROL + o), rd(c, AMDGPU_GRPH_PRIMARY_ADDR + o),
            rd(c, AMDGPU_GRPH_PRIMARY_HIGH + o), rd(c, AMDGPU_GRPH_PITCH + o),
            rd(c, AMDGPU_GRPH_X_START + o), rd(c, AMDGPU_GRPH_Y_START + o),
            rd(c, AMDGPU_GRPH_X_END + o), rd(c, AMDGPU_GRPH_Y_END + o),
            rd(c, AMDGPU_VIEWPORT_SIZE + o), rd(c, AMDGPU_GRPH_UPDATE + o),
        };
        struct amdgpu_grph g;

        amdgpu_decode_grph(&r, &g);

        if (!g.crtc_on) {
            continue;
        }

        lit++;

        begin(&line, i);
        say_text(&line, "controller ");
        say_dec(&line, k);
        say_text(&line, " on, ");
        say_dec(&line, g.view_w);
        say_text(&line, "x");
        say_dec(&line, g.view_h);
        say_text(&line, g.enabled ? ", graphics " : ", graphics OFF ");
        say_dec(&line, g.width);
        say_text(&line, "x");
        say_dec(&line, g.height);
        say_text(&line, " at ");
        say_dec(&line, g.bpp);
        say_text(&line, " bpp format ");
        say_dec(&line, g.format);
        say_text(&line, ", ");
        say_text(&line, amdgpu_array_mode_name(g.array_mode));
        say_text(&line, ", pitch ");
        say_dec(&line, g.pitch_px);
        say_text(&line, " px, surface MC ");
        say_hex(&line, (unsigned long)g.address, 10);
        say_text(&line, ", update ");
        say_hex(&line, r.update, 8);
        say_text(&line, ", frame ");
        say_dec(&line, rd(c, AMDGPU_CRTC_FRAME_COUNT + o));
        say_send(console, &line);

        if (c->crtc < 0 && g.enabled) {
            c->crtc = (int)k;
            c->grph = g;
        }
    }

    begin(&line, i);
    say_dec(&line, lit);
    say_text(&line, lit == 1 ? " display controller running" : " display controllers running");
    say_send(console, &line);

    if (c->crtc < 0) {
        return;
    }

    /* The controller that scans, held to the firmware's screen. */
    c->fb_in_bar = c->aperture != 0
                   && amdgpu_mc_to_bar(c->grph.address, c->hdp_base, c->aperture, &c->fb_offset);

    begin(&line, i);
    say_text(&line, "controller ");
    say_dec(&line, (unsigned long)c->crtc);
    say_text(&line, "'s surface is ");

    if (c->grph.address < c->fb_start || c->grph.address > c->fb_end) {
        say_text(&line, "OUTSIDE VRAM");
    } else if (!c->fb_in_bar) {
        say_text(&line, "in VRAM and not where BAR 0 shows (or the aperture is unknown)");
    } else {
        say_text(&line, "BAR 0 + ");
        say_hex(&line, (unsigned long)c->fb_offset, 8);
        if (c->have_vram) {
            say_text(&line, ", physical ");
            say_hex(&line, (unsigned long)(c->vram_dev.base + c->fb_offset), 10);
        }
    }

    if (machine_known && machine.screen_width != 0) {
        bool same = machine.screen_width == c->grph.width && machine.screen_height == c->grph.height
                    && machine.screen_pitch == c->grph.pitch_px * (c->grph.bpp / 8u);

        say_text(&line, same ? "; the firmware's screen, size and pitch the same"
                             : "; NOT the size or pitch the kernel's screen has: ");

        if (!same) {
            say_dec(&line, machine.screen_width);
            say_text(&line, "x");
            say_dec(&line, machine.screen_height);
            say_text(&line, " pitch ");
            say_dec(&line, machine.screen_pitch);
        }
    }

    say_send(console, &line);
}

/*
 * Where the work area goes: past the screen, rounded to a megabyte and a
 * megabyte more, on the card that shows it; from 16 MB on one that shows
 * nothing. Inside VRAM, inside BAR 0, and mapped.
 */
static void work_place(long console, unsigned i, struct card *c)
{
    struct say_line line;
    uint64_t at = 16u << 20, mc;
    long mapped;

    c->work_ok = false;

    if (!c->posted || !c->have_vram || c->aperture == 0) {
        return;
    }

    if (c->crtc >= 0 && c->fb_in_bar) {
        uint64_t end = c->fb_offset + (uint64_t)c->grph.pitch_px * (c->grph.bpp / 8u) * c->grph.height;

        at = ((end + 0xfffffu) & ~(uint64_t)0xfffffu) + 0x100000u;
    }

    mc = ((uint64_t)c->hdp_base << 8) + at;

    if (at + WORK_BYTES > c->aperture || mc < c->fb_start || mc + WORK_BYTES - 1u > c->fb_end) {
        begin(&line, i);
        say_text(&line, "no room for the work area at BAR 0 + ");
        say_hex(&line, (unsigned long)at, 8);
        say_send(console, &line);
        return;
    }

    if (c->work_at == 0 || c->work != at) {
        mapped = kosmos_dev_map((unsigned long)(c->vram_dev.base + at), WORK_BYTES / 4096u);

        if (mapped < 0) {
            begin(&line, i);
            say_text(&line, "the work area could not be mapped: ");
            say_dec(&line, (unsigned long)-mapped);
            say_send(console, &line);
            return;
        }

        c->work_at = (uintptr_t)mapped;
    }

    c->work = at;
    c->work_ok = true;

    /*
     * **Whether an engine's VRAM address reaches VRAM.** The DMA engine's
     * accesses go through the memory controller's VM as VMID 0 (`si_dma_
     * start` forces it). Linux's own setting is SYSTEM_ACCESS_MODE_NOT_IN_
     * SYS (`si_pcie_gart_enable`), under which an address inside the system
     * aperture - VRAM, as `si_mc_program` sets it - is not translated. So
     * the test runs when the L1 TLB is off, when the mode is PA_ONLY (0),
     * when context 0 is off - an inference: there is no page table for it
     * to use - or when the work area is inside the system aperture. Any
     * other state - a context 0 the firmware gave a page table, and the work
     * area outside the aperture - and the address could land anywhere, RAM
     * too, so nothing is tried.
     */
    c->untranslated = (c->vm_tlb & 1u) == 0 || ((c->vm_tlb >> 3) & 3u) == 0u
                      || (c->vm_ctx0 & AMDGPU_ENABLE_CONTEXT) == 0
                      || (c->sys_set && mc >= c->sys_low && mc + WORK_BYTES - 1u <= c->sys_high);

    begin(&line, i);
    say_text(&line, "work area at BAR 0 + ");
    say_hex(&line, (unsigned long)at, 8);
    say_text(&line, ", MC ");
    say_hex(&line, (unsigned long)mc, 10);
    say_text(&line, c->untranslated ? " - an engine's address there is VRAM"
                                    : " - an engine's address there may be TRANSLATED: no DMA test");
    say_send(console, &line);
}

static void look_card(long console, unsigned i)
{
    struct card *c = &CARD[i];

    if (c->regs == 0) {
        return;
    }

    look_chip(console, i, c);

    if (!c->answers) {
        return;
    }

    look_display(console, i, c);
    work_place(console, i, c);
}

/*--------------------------------------------------------------------------
 * **A DMA engine's first work** (`docs/amdgpu.md` step 3a): its ring set up
 * as `si_dma_start` does, in VRAM; four commands - a number written, 64 KB
 * filled, the 64 KB copied, a fence - the write pointer moved, and then
 * whether each happened. The ring is switched off again after, so every test
 * starts from the same place and nothing is left running.
 *------------------------------------------------------------------------*/

static uint64_t work_mc(const struct card *c, uint32_t offset)
{
    return ((uint64_t)c->hdp_base << 8) + c->work + offset;
}

static void engine_test(long console, unsigned i, unsigned e, struct engine_result *res)
{
    struct card *c = &CARD[i];
    struct say_line line;
    const uint32_t at = e ? AMDGPU_DMA1_OFFSET : 0u;
    const uint32_t ring = e ? WORK_RING1 : WORK_RING0;
    const uint32_t words = WORK_WORDS + e * 0x100u;
    const uint32_t src = WORK_TEST + e * 0x20000u, dst = src + TEST_BYTES;
    uint32_t cmd[24], n = 0, k, rb_cntl, reset;
    unsigned long t0, t1 = 0, waited;

    memset(res, 0, sizeof *res);

    reset = rd(c, AMDGPU_SRBM_SOFT_RESET) & (e ? AMDGPU_SOFT_RESET_DMA1 : AMDGPU_SOFT_RESET_DMA);
    rb_cntl = rd(c, AMDGPU_DMA_RB_CNTL + at);

    if (!c->posted || !c->work_ok || !c->untranslated || reset != 0 || (rb_cntl & AMDGPU_DMA_RB_ENABLE)) {
        begin(&line, i);
        say_text(&line, "DMA ");
        say_dec(&line, e);
        say_text(&line, " not tried: ");
        say_text(&line, !c->posted ? "the card is not posted"
                        : !c->work_ok ? "no work area in VRAM"
                        : !c->untranslated ? "its addresses may be translated"
                        : reset ? "the engine is held in soft reset"
                        : "its ring is already on - somebody else's");
        say_send(console, &line);
        res->error = GPU_ERR_NO_RING;
        return;
    }

    /* The words and the test's source and destination, cleared. */
    vram_wr(c, words, 0xcafedeadu);
    vram_wr(c, words + 4, 0);

    for (k = 0; k < TEST_BYTES; k += 4) {
        vram_wr(c, src + k, 0);
        vram_wr(c, dst + k, 0);
    }

    n += amdgpu_dma_write(cmd + n, work_mc(c, words), WRITE_VALUE);
    n += amdgpu_dma_fill(cmd + n, work_mc(c, src), TEST_PATTERN, TEST_BYTES);
    n += amdgpu_dma_copy(cmd + n, work_mc(c, dst), work_mc(c, src), TEST_BYTES);
    n += amdgpu_dma_fence(cmd + n, work_mc(c, words + 4), 1);

    while ((n & 7u) != 0) {
        n += amdgpu_dma_nop(cmd + n);
    }

    for (k = 0; k < RING_BYTES; k += 4) {
        vram_wr(c, ring + k, k / 4 < n ? cmd[k / 4] : 0u);
    }

    hdp_flush(c);

    /* `si_dma_start`, in its order. */
    wr(c, AMDGPU_DMA_SEM_INCOMPLETE + at, 0);
    wr(c, AMDGPU_DMA_SEM_WAIT_FAIL + at, 0);
    rb_cntl = amdgpu_dma_rb_size(RING_BYTES);
    wr(c, AMDGPU_DMA_RB_CNTL + at, rb_cntl);
    wr(c, AMDGPU_DMA_RB_RPTR + at, 0);
    wr(c, AMDGPU_DMA_RB_WPTR + at, 0);
    wr(c, AMDGPU_DMA_RB_RPTR_ADDR_LO + at, (uint32_t)work_mc(c, words + 0x80u));
    wr(c, AMDGPU_DMA_RB_RPTR_ADDR_HI + at, (uint32_t)(work_mc(c, words + 0x80u) >> 32) & 0xffu);
    wr(c, AMDGPU_DMA_RB_BASE + at, (uint32_t)(work_mc(c, ring) >> 8));
    wr(c, AMDGPU_DMA_IB_CNTL + at, AMDGPU_DMA_IB_ENABLE | AMDGPU_CMD_VMID_FORCE);
    wr(c, AMDGPU_DMA_CNTL + at, rd(c, AMDGPU_DMA_CNTL + at) & ~AMDGPU_CTXEMPTY_INT);
    wr(c, AMDGPU_DMA_RB_WPTR + at, 0);
    wr(c, AMDGPU_DMA_RB_CNTL + at, rb_cntl | AMDGPU_DMA_RB_ENABLE);

    t0 = kosmos_ticks();
    wr(c, AMDGPU_DMA_RB_WPTR + at, amdgpu_dma_wptr(n));

    for (waited = 0; waited <= WAIT_TICKS; waited++) {
        hdp_invalidate(c);

        if (vram_rd(c, words + 4) == 1u) {
            t1 = kosmos_ticks();
            break;
        }

        kosmos_sleep(1);
    }

    hdp_invalidate(c);
    res->value = vram_rd(c, words);
    res->fence = vram_rd(c, words + 4);
    res->rptr = rd(c, AMDGPU_DMA_RB_RPTR + at);
    res->wptr = rd(c, AMDGPU_DMA_RB_WPTR + at);
    res->rb_cntl = rd(c, AMDGPU_DMA_RB_CNTL + at);
    res->status = rd(c, AMDGPU_DMA_STATUS + at);
    res->srbm2 = rd(c, AMDGPU_SRBM_STATUS2);
    res->elapsed_us = t1 ? (uint32_t)counter_us(t0, t1) : 0u;

    for (k = 0; k < TEST_BYTES; k += 4) {
        res->filled += vram_rd(c, src + k) == TEST_PATTERN;
        res->copied += vram_rd(c, dst + k) == TEST_PATTERN;
    }

    /* Off again: `si_dma_stop`. */
    wr(c, AMDGPU_DMA_RB_CNTL + at, rd(c, AMDGPU_DMA_RB_CNTL + at) & ~AMDGPU_DMA_RB_ENABLE);

    res->error = res->fence != 1u ? GPU_ERR_NO_ANSWER
               : res->value != WRITE_VALUE || res->filled != TEST_BYTES / 4u || res->copied != TEST_BYTES / 4u
                 ? GPU_ERR_UNCHANGED : GPU_OK;

    begin(&line, i);
    say_text(&line, "DMA ");
    say_dec(&line, e);
    say_text(&line, res->error == GPU_OK ? " works: " : " FAILED: ");
    say_text(&line, "word ");
    say_hex(&line, res->value, 8);
    say_text(&line, ", fence ");
    say_dec(&line, res->fence);
    say_text(&line, ", filled ");
    say_dec(&line, res->filled);
    say_text(&line, " and copied ");
    say_dec(&line, res->copied);
    say_text(&line, " of 16384 words, ");
    say_dec(&line, res->elapsed_us);
    say_text(&line, " us; rptr ");
    say_hex(&line, res->rptr, 5);
    say_text(&line, " wptr ");
    say_hex(&line, res->wptr, 5);
    say_text(&line, ", status ");
    say_hex(&line, res->status, 8);
    say_text(&line, ", SRBM2 ");
    say_hex(&line, res->srbm2, 8);
    say_send(console, &line);
}

/*--------------------------------------------------------------------------
 * **A page flip, and back** (`docs/amdgpu.md` step 2b): a picture of its
 * own drawn in VRAM past the work area, the controller's primary surface
 * pointed at it as `evergreen_page_flip` does - the high word, then the low,
 * which arms the update for the next vertical blank - shown for `seconds`,
 * and the firmware's surface put back the same way. The pitch is the
 * screen's, so only the address is written.
 *------------------------------------------------------------------------*/

static uint32_t picture(unsigned x, unsigned y, unsigned w, unsigned h)
{
    static const uint32_t BARS[8] = { 0xffffffu, 0xffff00u, 0x00ffffu, 0x00ff00u,
                                      0xff00ffu, 0xff0000u, 0x0000ffu, 0x202020u };

    if (x < 8 || y < 8 || x + 8 >= w || y + 8 >= h) {
        return 0xff8000u;                       /* an orange border: the edges are where they should be */
    }

    return BARS[(x * 8u) / w];
}

static bool flip_wait(const struct card *c, uint32_t o, unsigned long *frames)
{
    uint32_t f0 = rd(c, AMDGPU_CRTC_FRAME_COUNT + o);
    unsigned waited;

    for (waited = 0; waited < 25u; waited++) {
        if ((rd(c, AMDGPU_GRPH_UPDATE + o) & AMDGPU_GRPH_UPDATE_PENDING) == 0) {
            *frames = rd(c, AMDGPU_CRTC_FRAME_COUNT + o) - f0;
            return true;
        }

        kosmos_sleep(1);
    }

    *frames = rd(c, AMDGPU_CRTC_FRAME_COUNT + o) - f0;
    return false;
}

static void flip_test(long console, unsigned seconds, struct gpu_reply *rep)
{
    struct say_line line;
    struct card *c = NULL;
    unsigned i, x, y;
    uint32_t o, old_lo, old_hi, new_lo, new_hi, during, after;
    uint64_t at, mc, bytes, row_bytes;
    unsigned long frames_in = 0, frames_out = 0;
    bool took, back;

    for (i = 0; i < ncards; i++) {
        if (CARD[i].crtc >= 0 && CARD[i].fb_in_bar && CARD[i].work_ok) {
            c = &CARD[i];
            break;
        }
    }

    if (c == NULL || c->grph.bpp != 32 || !c->grph.linear || c->grph.width == 0 || c->grph.height == 0) {
        say(console, "amdgpu: no flip test - no card shows a linear 32 bpp screen inside its BAR 0\n");
        rep->error = c == NULL ? GPU_ERR_NO_PIPE : GPU_ERR_NO_BUFFER;
        return;
    }

    row_bytes = (uint64_t)c->grph.pitch_px * 4u;
    bytes = row_bytes * c->grph.height;
    at = (c->work + WORK_BYTES + 0xfffffu) & ~(uint64_t)0xfffffu;
    mc = ((uint64_t)c->hdp_base << 8) + at;

    if (at + bytes > c->aperture || mc + bytes - 1u > c->fb_end) {
        say(console, "amdgpu: no flip test - no room in BAR 0 for a second screen\n");
        rep->error = GPU_ERR_NO_MEMORY;
        return;
    }

    /* The picture, a band of rows at a time: one mapping is at most 16 MB. */
    for (y = 0; y < c->grph.height;) {
        unsigned rows = (unsigned)((4096u * 4096u) / row_bytes), r;
        uint64_t first = at + (uint64_t)y * row_bytes, page = first & ~(uint64_t)0xfffu;
        unsigned long pages;
        long mapped;

        if (rows > c->grph.height - y) {
            rows = c->grph.height - y;
        }

        pages = (unsigned long)((first - page + rows * row_bytes + 0xfffu) / 4096u);

        if (pages > 4096u) {
            rows--;
            continue;
        }

        mapped = kosmos_dev_map((unsigned long)(c->vram_dev.base + page), pages);

        if (mapped < 0) {
            say(console, "amdgpu: no flip test - the second screen could not be mapped\n");
            rep->error = GPU_ERR_NO_MEMORY;
            return;
        }

        for (r = 0; r < rows; r++) {
            uintptr_t row = (uintptr_t)mapped + (uintptr_t)(first - page) + (uintptr_t)(r * row_bytes);

            for (x = 0; x < c->grph.width; x++) {
                mmio_write32(row + x * 4u, picture(x, y + r, c->grph.width, c->grph.height));
            }
        }

        (void)kosmos_share_unmap((unsigned long)mapped, pages);
        y += rows;
    }

    hdp_flush(c);

    o = amdgpu_crtc_offset((unsigned)c->crtc);
    old_lo = rd(c, AMDGPU_GRPH_PRIMARY_ADDR + o);
    old_hi = rd(c, AMDGPU_GRPH_PRIMARY_HIGH + o);
    new_lo = (uint32_t)mc;
    new_hi = (uint32_t)(mc >> 32) & 0xffu;

    wr(c, AMDGPU_GRPH_PRIMARY_HIGH + o, new_hi);
    wr(c, AMDGPU_GRPH_PRIMARY_ADDR + o, new_lo);
    during = rd(c, AMDGPU_GRPH_PRIMARY_ADDR + o);
    took = flip_wait(c, o, &frames_in);
    c->flips++;

    kosmos_sleep((unsigned long)seconds * (machine_known && machine.tick_hz ? machine.tick_hz : 250u));

    wr(c, AMDGPU_GRPH_PRIMARY_HIGH + o, old_hi);
    wr(c, AMDGPU_GRPH_PRIMARY_ADDR + o, old_lo);
    back = flip_wait(c, o, &frames_out);
    after = rd(c, AMDGPU_GRPH_PRIMARY_ADDR + o);
    c->flips++;

    rep->error = took && back && during == new_lo && after == old_lo ? GPU_OK : GPU_ERR_UNCHANGED;
    rep->words[0] = old_lo;
    rep->words[1] = new_lo;
    rep->words[2] = during;
    rep->words[3] = after;
    rep->words[4] = (uint32_t)frames_in;
    rep->words[5] = (uint32_t)frames_out;
    rep->words[6] = rd(c, AMDGPU_GRPH_UPDATE + o);

    say_begin(&line);
    say_text(&line, "amdgpu: flip to MC ");
    say_hex(&line, (unsigned long)mc, 10);
    say_text(&line, took ? " taken in " : " NOT TAKEN after ");
    say_dec(&line, frames_in);
    say_text(&line, " frames, back ");
    say_text(&line, back ? "in " : "NOT TAKEN after ");
    say_dec(&line, frames_out);
    say_text(&line, "; surface read ");
    say_hex(&line, during, 8);
    say_text(&line, " then ");
    say_hex(&line, after, 8);
    say_text(&line, after == old_lo ? " - the firmware's again" : " - NOT the firmware's");
    say_send(console, &line);
}

/*--------------------------------------------------------------------------
 * Answered.
 *------------------------------------------------------------------------*/

static void look(long console)
{
    unsigned i;

    for (i = 0; i < ncards; i++) {
        look_card(console, i);
    }
}

static void engines(long console, unsigned e, struct gpu_reply *rep)
{
    unsigned i;
    bool first = true;

    rep->error = GPU_ERR_NO_DEVICE;

    for (i = 0; i < ncards; i++) {
        struct engine_result res;

        if (CARD[i].regs == 0 || !CARD[i].answers) {
            continue;
        }

        engine_test(console, i, e, &res);

        if (first || (rep->error != GPU_OK && res.error == GPU_OK)) {
            rep->error = res.error;
            rep->engine.value = res.value;
            rep->engine.head = res.rptr;
            rep->engine.tail = res.wptr;
            rep->engine.ctl = res.rb_cntl;
            rep->engine.mode = res.status;
            rep->engine.esr = res.srbm2;
            rep->engine.elapsed_us = res.elapsed_us;
            first = false;
        }
    }
}

static void stats(struct gpu_stats *st)
{
    const struct card *c = NULL;
    unsigned i;

    memset(st, 0, sizeof *st);
    st->shown = ~0u;

    for (i = 0; i < ncards; i++) {
        if (CARD[i].answers && (c == NULL || CARD[i].crtc >= 0)) {
            c = &CARD[i];
        }
    }

    if (c == NULL) {
        st->error = GPU_ERR_NO_DEVICE;
        return;
    }

    st->device = c->device;
    st->flips = c->flips;

    if (c->crtc >= 0) {
        uint32_t o = amdgpu_crtc_offset((unsigned)c->crtc);

        st->width = c->grph.width;
        st->height = c->grph.height;
        st->frames = rd(c, AMDGPU_CRTC_FRAME_COUNT + o);
        st->shown = 0;
        st->nbuffers = 1;
        st->buffers[0].id = 0;
        st->buffers[0].holder = 0;                  /* the firmware */
        st->buffers[0].width = c->grph.width;
        st->buffers[0].height = c->grph.height;
        st->buffers[0].pages = (uint32_t)(((uint64_t)c->grph.pitch_px * (c->grph.bpp / 8u)
                                           * c->grph.height + 4095u) / 4096u);
        st->buffers[0].kind = GPU_BUFFER_SCANOUT;
    }
}

/*
 * **The door**: false when the board knows no Southern Islands GPU - at
 * once and silent when it knows no AMD GPU at all - so the role can go on
 * to `intelgpu_server`; otherwise it never returns.
 */
bool amdgpu_server(long console, long endpoint)
{
    struct say_line line;
    unsigned i;

    ncards = 0;

    for (i = 0; i < CARDS_MAX; i++) {
        if (!find(console, i, &CARD[ncards])) {
            break;
        }

        ncards++;
    }

    /* Only a Southern Islands card takes the door: a machine with another
     * AMD GPU beside an Intel one keeps the Intel driver. */
    for (i = 0; i < ncards && CARD[i].family == AMDGPU_NOT_SI; i++) {
    }

    if (ncards == 0 || i == ncards) {
        return false;
    }

    (void)kosmos_setname("amdgpu", 6);       /* the role started as the Intel driver's */
    machine_known = kosmos_sysinfo(&machine) == 0;
    look(console);

    /* The DMA engines' first work, wherever it is safe to try (step 3a). */
    for (i = 0; i < AMDGPU_DMA_ENGINES; i++) {
        struct gpu_reply rep;

        memset(&rep, 0, sizeof rep);
        engines(console, i, &rep);
    }

    say_begin(&line);
    say_text(&line, "amdgpu: ");
    say_dec(&line, ncards);
    say_text(&line, ncards == 1 ? " card; the screen is the firmware's, untouched - `gpu cursor 5` flips it"
                                : " cards; the screen is the firmware's, untouched - `gpu cursor 5` flips it");
    say_send(console, &line);

    for (;;) {
        static struct message msg, out;
        uint64_t sender = 0;
        struct gpu_request rq;
        struct gpu_reply rep;
        long cap;

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
                machine_known = kosmos_sysinfo(&machine) == 0;
                look(console);
                rep.error = GPU_OK;
                break;
            case GPU_OP_STATS:
                break;
            case GPU_OP_ENGINE_TEST:
                engines(console, rq.x == 1 ? 1u : 0u, &rep);
                break;
            case GPU_OP_CURSOR_TEST:
                flip_test(console, rq.seconds < 1 ? 1u : rq.seconds > 30 ? 30u : rq.seconds, &rep);
                break;
            case GPU_OP_FLIPPED: {
                const struct card *c = NULL;

                for (i = 0; i < ncards; i++) {
                    if (CARD[i].crtc >= 0) {
                        c = &CARD[i];
                        break;
                    }
                }

                if (c == NULL) {
                    rep.error = GPU_ERR_NO_PIPE;
                } else {
                    rep.flip.flips = c->flips;
                    rep.flip.live = rd(c, AMDGPU_GRPH_PRIMARY_ADDR + amdgpu_crtc_offset((unsigned)c->crtc));
                    rep.flip.shown = ~0u;
                }
                break;
            }
            default:
                rep.error = GPU_ERR_NO_DEVICE;     /* what the window manager falls back on */
                break;
            }
        }

        if (cap >= 0) {
            (void)kosmos_cap_drop(cap);
        }

        memset(&out, 0, sizeof out);
        out.tag = msg.tag;

        if (msg.length >= 8 && rq.op == GPU_OP_STATS) {
            static struct gpu_stats st;

            stats(&st);
            out.length = (uint32_t)sizeof st;
            memcpy(out.data, &st, sizeof st);
        } else {
            out.length = (uint32_t)sizeof rep;
            memcpy(out.data, &rep, sizeof rep);
        }

        (void)kosmos_reply(sender, &out);
    }
}
