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
#include "amdgpu_cp.h"
#include "amdgpu_decode.h"
#include "amdgpu_fw.h"
#include "amdgpu_ih.h"
#include "amdgpu_vm.h"
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
#define WORK_IH                 0x50000u    /* the interrupt ring, AMDGPU_IH_RING_BYTES (V1) */
#define WORK_IH_DUMMY           0x60000u    /* the IH's dummy read */
#define WORK_GART               0x61000u    /* the GART's table: GTT_BYTES / 4096 entries (V2) */
#define WORK_PD1                0x81000u    /* VMID 1's page directory, its one table after */
#define WORK_PT1                0x82000u
#define WORK_PD2                0x8a000u    /* VMID 2's */
#define WORK_PT2                0x8b000u
#define WORK_GFX_RING           0x93000u    /* the CP's ring 0 (V3) */
#define WORK_RLC_SR             0x94000u    /* the RLC's save-and-restore and clear-state pages */
#define WORK_RLC_CS             0x95000u
#define WORK_BYTES              0x96000u
#define RING_BYTES              0x1000u
#define TEST_BYTES              0x10000u
#define TEST_PATTERN            0x00c0ffeeu
#define WRITE_VALUE             0xdeadbeefu
#define WAIT_TICKS              50u         /* a DMA test's longest wait: 200 ms at 250 a second */
#define GTT_BYTES               (64u << 20) /* the GART's span: 16384 pages, a 128 KB table */
#define VM_GIGABYTES            8u          /* contexts 1-15: radeon's default, a 4 KB directory */
#define GFX_RING_BYTES          0x1000u
#define SYS_PAGES               8u          /* system pages the V2 test writes to through the GPU */
#define VA_TARGET               0x100000u   /* in VMIDs 1 and 2: a page each, a different one */
#define VA_IBS                  0x101000u   /* in both: the page of indirect buffers */
#define VA_UNMAPPED             0x200000u   /* in neither: the fault's control */

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
    /* V1: interrupts. */
    long            irq;                /* the MSI's capability, or -1: entries are polled */
    bool            ih_on;
    uint32_t        ih_rptr;
    unsigned long   ih_entries;         /* read since the ring went on */
    /* V2: the GART and programs' address spaces. */
    bool            gart_on;
    uint64_t        gtt_start, gtt_end;
    struct amdgpu_vm_geometry vm;
    uint8_t        *sys;                /* SYS_PAGES of system memory, mapped */
    uint64_t        sys_bus;            /* where they are, to the GPU */
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

/*
 * A register past the 76 KB mapped - DB_DEPTH_INFO, at 0x2803c, which the
 * RLC's stop reads - through MM_INDEX and MM_DATA, at 0 and 4, as radeon's
 * `r100_mm_rreg` does beyond its mapping (`radeon.h` 2455-2470,
 * `radeon_reg.h` 1228-1229). One thread asks, so the pair needs no lock.
 */
static uint32_t rd_any(const struct card *c, uint32_t offset)
{
    if (offset < AMDGPU_REGS_BYTES) {
        return rd(c, offset);
    }

    wr(c, 0x0u, offset);
    return rd(c, 0x4u);
}

static void wr_any(const struct card *c, uint32_t offset, uint32_t value)
{
    if (offset < AMDGPU_REGS_BYTES) {
        wr(c, offset, value);
        return;
    }

    wr(c, 0x0u, offset);
    wr(c, 0x4u, value);
}

static bool counter_known(void)
{
    return machine_known && machine.counter_hz >= 1000000u;
}

/* Linux's udelay: on the counter, and a tick - longer, which is safe -
 * where the counter's rate is not known. */
static void spin_us(uint32_t us)
{
    unsigned long t0 = kosmos_ticks();

    if (!counter_known()) {
        kosmos_sleep(1);
        return;
    }

    while (counter_us(t0, kosmos_ticks()) < us) {
    }
}

/*
 * A register read until it answers, for at most `us`: spun for the first
 * millisecond, as Linux's loops of udelay(1) are, and a tick apart after,
 * so a register that never answers costs the core a millisecond and the
 * request its bound.
 */
static bool poll_reg(const struct card *c, uint32_t reg, uint32_t mask, uint32_t value, uint32_t us)
{
    unsigned long t0 = kosmos_ticks(), slept = 0;
    unsigned long bound_ticks = 1u + us / 4000u;        /* ticks are 4 ms at the least rate here */

    for (;;) {
        unsigned long el;

        if ((rd_any(c, reg) & mask) == value) {
            return true;
        }

        el = counter_known() ? counter_us(t0, kosmos_ticks()) : 1000u + slept * 4000u;

        if (el >= us || slept > bound_ticks) {
            return false;
        }

        if (el >= 1000u) {
            kosmos_sleep(1);
            slept++;
        }
    }
}

/* A program (`amdgpu_ih.h`) performed: -1, or the entry it stopped at. */
static int run(const struct card *c, const struct amdgpu_prog *p)
{
    unsigned k;

    if (p->overflow) {
        return 0;
    }

    for (k = 0; k < p->n; k++) {
        uint32_t reg = p->w[k].reg, value = p->w[k].value;

        if (reg == AMDGPU_WR_DELAY) {
            spin_us(value);
        } else if (reg == AMDGPU_WR_POLL) {
            if (k + 1u >= p->n
                || !poll_reg(c, value, p->w[k + 1u].reg, p->w[k + 1u].value, AMDGPU_POLL_US)) {
                return (int)k;
            }
            k++;
        } else {
            wr_any(c, reg, value);
        }
    }

    return -1;
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
    c->irq = -1;

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

    /* Once the interrupt ring or the GART's table is in it, it stays: the
     * card is reading there. */
    if ((c->ih_on || c->gart_on) && c->work_at != 0) {
        at = c->work;
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

/*--------------------------------------------------------------------------
 * **Interrupts** (`docs/amdgpu.md` V1): the IH's ring, 64 KB in the work
 * area, set up as `si_ih_irq_init` does, every source but the engines'
 * masked as `si_disable_interrupt_state` leaves them - so it is on from boot
 * and nothing raises an entry until a test asks for one. The MSI is claimed
 * when the board told one (`dev_info.intid`); without it, entries are read
 * by polling the write pointer, which is what radeon does without write-back.
 *------------------------------------------------------------------------*/

static void ih_start(long console, unsigned i)
{
    struct card *c = &CARD[i];
    struct say_line line;
    struct amdgpu_ih_cfg cfg;
    struct amdgpu_prog p;
    uint32_t k;
    int stop;

    if (c->ih_on || !c->work_ok || !c->untranslated) {
        return;
    }

    if (rd(c, AMDGPU_IH_RB_CNTL) & AMDGPU_IH_RB_ENABLE) {
        begin(&line, i);
        say_text(&line, "IH already on - somebody else's; left alone, and no interrupts here");
        say_send(console, &line);
        return;
    }

    for (k = 0; k < AMDGPU_IH_RING_BYTES; k += 4) {
        vram_wr(c, WORK_IH + k, 0);
    }
    for (k = 0; k < 4096u; k += 4) {
        vram_wr(c, WORK_IH_DUMMY + k, 0);
    }
    hdp_flush(c);

    if (c->irq < 0 && c->regs_dev.intid != 0) {
        c->irq = kosmos_irq_claim(c->regs_dev.intid);
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.ring_mc = work_mc(c, WORK_IH);
    cfg.ring_bytes = AMDGPU_IH_RING_BYTES;
    cfg.wptr_mc = work_mc(c, WORK_WORDS + 0xf0u);
    cfg.writeback = false;
    cfg.dummy_mc = work_mc(c, WORK_IH_DUMMY);
    cfg.in_vram = true;
    cfg.msi = c->irq >= 0;
    cfg.crtcs = amdgpu_family_info(c->family)->crtcs;
    cfg.ih_rb_cntl = rd(c, AMDGPU_IH_RB_CNTL);
    cfg.ih_cntl = rd(c, AMDGPU_IH_CNTL);
    cfg.interrupt_cntl = rd(c, AMDGPU_INTERRUPT_CNTL);
    cfg.cp_int_cntl_ring0 = rd(c, AMDGPU_CP_INT_CNTL_RING0);
    cfg.dma_cntl[0] = rd(c, AMDGPU_DMA_CNTL);
    cfg.dma_cntl[1] = rd(c, AMDGPU_DMA_CNTL + AMDGPU_DMA1_OFFSET);

    if (!amdgpu_ih_setup(&p, &cfg) || (stop = run(c, &p)) >= 0) {
        begin(&line, i);
        say_text(&line, "IH NOT set up: the program was refused");
        say_send(console, &line);
        return;
    }

    c->ih_on = true;
    c->ih_rptr = 0;
    c->ih_entries = 0;

    begin(&line, i);
    say_text(&line, "IH on: a 64 KB ring at MC ");
    say_hex(&line, (unsigned long)cfg.ring_mc, 10);
    if (c->irq >= 0) {
        say_text(&line, ", its MSI vector ");
        say_dec(&line, c->regs_dev.intid);
    } else {
        say_text(&line, c->regs_dev.intid != 0 ? ", its MSI NOT CLAIMED - entries are polled"
                                               : ", no MSI told by the board - entries are polled");
    }
    say_send(console, &line);
}

/*
 * The entries since the last read, read: each counted, the first `say` of
 * them said, and `want`'s first copied out. The read pointer is written
 * back, and an overflow cleared as `si_get_ih_wptr` clears it.
 */
static unsigned ih_drain(long console, unsigned i, uint32_t want, struct amdgpu_iv *found,
                         unsigned say)
{
    struct card *c = &CARD[i];
    uint32_t raw, wptr, rptr, dw[4], k;
    unsigned n = 0, seen = 0;
    bool overflow;

    if (!c->ih_on) {
        return 0;
    }

    hdp_invalidate(c);
    raw = rd(c, AMDGPU_IH_RB_WPTR);
    wptr = amdgpu_ih_wptr(raw, AMDGPU_IH_RING_BYTES, &overflow);
    rptr = c->ih_rptr;

    if (overflow) {
        uint32_t cntl = rd(c, AMDGPU_IH_RB_CNTL);

        rptr = (wptr + AMDGPU_IV_BYTES) & (AMDGPU_IH_RING_BYTES - 1u);
        wr(c, AMDGPU_IH_RB_CNTL, cntl | AMDGPU_IH_WPTR_OVERFLOW_CLEAR);
        wr(c, AMDGPU_IH_RB_CNTL, cntl & ~AMDGPU_IH_WPTR_OVERFLOW_CLEAR);
    }

    while (rptr != wptr && n < AMDGPU_IH_RING_BYTES / AMDGPU_IV_BYTES) {
        struct amdgpu_iv iv;

        for (k = 0; k < 4; k++) {
            dw[k] = vram_rd(c, WORK_IH + rptr + k * 4u);
        }
        amdgpu_ih_decode(dw, &iv);
        n++;

        if (iv.src_id == want && seen == 0 && found != NULL) {
            *found = iv;
            seen = 1;
        }

        if (n <= say) {
            struct say_line line;
            const char *name = amdgpu_ih_source_name(iv.src_id);

            begin(&line, i);
            say_text(&line, "IH entry: source ");
            say_dec(&line, iv.src_id);
            say_text(&line, name != NULL ? " (" : "");
            say_text(&line, name != NULL ? name : "");
            say_text(&line, name != NULL ? ")" : "");
            say_text(&line, ", data ");
            say_hex(&line, iv.src_data, 7);
            say_text(&line, ", ring ");
            say_dec(&line, iv.ring_id);
            say_text(&line, ", VMID ");
            say_dec(&line, iv.vmid);
            say_send(console, &line);
        }

        rptr = amdgpu_ih_next(rptr, AMDGPU_IH_RING_BYTES);
    }

    wr(c, AMDGPU_IH_RB_RPTR, rptr);
    c->ih_rptr = rptr;
    c->ih_entries += n;
    return seen;
}

/*
 * An entry from `want` waited for, at most `ticks`: the MSI where there is
 * one - and whether it was heard - and the ring read after each wake or
 * each tick.
 */
static bool ih_wait(long console, unsigned i, uint32_t want, unsigned ticks,
                    struct amdgpu_iv *found, bool *heard)
{
    struct card *c = &CARD[i];
    unsigned t;

    *heard = false;

    for (t = 0; t <= ticks; t++) {
        if (c->irq >= 0) {
            if (kosmos_irq_wait_for(c->irq, 1) >= 0) {
                *heard = true;
                (void)kosmos_irq_ack(c->irq);
            }
        } else if (t > 0) {
            kosmos_sleep(1);
        }

        if (ih_drain(console, i, want, found, 4)) {
            return true;
        }
    }

    return false;
}

/*--------------------------------------------------------------------------
 * A DMA engine's ring started and stopped as `si_dma_start` and
 * `si_dma_stop` do: in VRAM, its read pointer written back beside the
 * engine's words, `n` dwords of commands already in it. Every test of an
 * engine goes through these two.
 *------------------------------------------------------------------------*/

static void dma_start(const struct card *c, unsigned e, uint32_t ring, uint32_t words, uint32_t n)
{
    const uint32_t at = e ? AMDGPU_DMA1_OFFSET : 0u;
    uint32_t rb_cntl = amdgpu_dma_rb_size(RING_BYTES), cntl;

    wr(c, AMDGPU_DMA_SEM_INCOMPLETE + at, 0);
    wr(c, AMDGPU_DMA_SEM_WAIT_FAIL + at, 0);
    wr(c, AMDGPU_DMA_RB_CNTL + at, rb_cntl);
    wr(c, AMDGPU_DMA_RB_RPTR + at, 0);
    wr(c, AMDGPU_DMA_RB_WPTR + at, 0);
    wr(c, AMDGPU_DMA_RB_RPTR_ADDR_LO + at, (uint32_t)work_mc(c, words + 0x80u));
    wr(c, AMDGPU_DMA_RB_RPTR_ADDR_HI + at, (uint32_t)(work_mc(c, words + 0x80u) >> 32) & 0xffu);
    wr(c, AMDGPU_DMA_RB_BASE + at, (uint32_t)(work_mc(c, ring) >> 8));
    wr(c, AMDGPU_DMA_IB_CNTL + at, AMDGPU_DMA_IB_ENABLE | AMDGPU_CMD_VMID_FORCE);
    cntl = rd(c, AMDGPU_DMA_CNTL + at) & ~AMDGPU_CTXEMPTY_INT;
    /* The trap reaches the IH only with TRAP_ENABLE (`si_irq_set`, 6060-6089). */
    cntl = c->ih_on ? cntl | AMDGPU_DMA_TRAP_ENABLE : cntl & ~AMDGPU_DMA_TRAP_ENABLE;
    wr(c, AMDGPU_DMA_CNTL + at, cntl);
    wr(c, AMDGPU_DMA_RB_WPTR + at, 0);
    wr(c, AMDGPU_DMA_RB_CNTL + at, rb_cntl | AMDGPU_DMA_RB_ENABLE);
    wr(c, AMDGPU_DMA_RB_WPTR + at, amdgpu_dma_wptr(n));
}

static void dma_stop(const struct card *c, unsigned e)
{
    const uint32_t at = e ? AMDGPU_DMA1_OFFSET : 0u;

    wr(c, AMDGPU_DMA_RB_CNTL + at, rd(c, AMDGPU_DMA_RB_CNTL + at) & ~AMDGPU_DMA_RB_ENABLE);
    wr(c, AMDGPU_DMA_CNTL + at, rd(c, AMDGPU_DMA_CNTL + at) & ~AMDGPU_DMA_TRAP_ENABLE);
}

/* The commands into the ring, padded to eight and the rest nought. */
static void dma_fill_ring(const struct card *c, uint32_t ring, const uint32_t *cmd, uint32_t n)
{
    uint32_t k;

    for (k = 0; k < RING_BYTES; k += 4) {
        vram_wr(c, ring + k, k / 4 < n ? cmd[k / 4] : 0u);
    }
    hdp_flush(c);
}

/* A word of the work area waited for, at most WAIT_TICKS. */
static bool word_wait(const struct card *c, uint32_t offset, uint32_t value, unsigned long *t1)
{
    unsigned long waited;

    for (waited = 0; waited <= WAIT_TICKS; waited++) {
        hdp_invalidate(c);

        if (vram_rd(c, offset) == value) {
            *t1 = kosmos_ticks();
            return true;
        }

        kosmos_sleep(1);
    }

    return false;
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
    unsigned long t0, t1 = 0;
    struct amdgpu_iv iv;
    bool trapped = false, heard = false;

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

    /* V1: the fence's interrupt, as `si_dma_ring_emit_fence` raises it. */
    if (c->ih_on) {
        (void)ih_drain(console, i, ~0u, NULL, 0);       /* whatever came before, not this test's */
        n += amdgpu_dma_trap(cmd + n);
    }

    while ((n & 7u) != 0) {
        n += amdgpu_dma_nop(cmd + n);
    }

    dma_fill_ring(c, ring, cmd, n);

    t0 = kosmos_ticks();
    dma_start(c, e, ring, words, n);
    (void)word_wait(c, words + 4, 1u, &t1);

    if (c->ih_on) {
        trapped = ih_wait(console, i, e ? AMDGPU_SRC_DMA1_TRAP : AMDGPU_SRC_DMA0_TRAP, 10, &iv, &heard);
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
    dma_stop(c, e);

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

    if (c->ih_on) {
        begin(&line, i);
        say_text(&line, "DMA ");
        say_dec(&line, e);
        if (trapped) {
            say_text(&line, "'s trap reached the IH: source ");
            say_dec(&line, iv.src_id);
            say_text(&line, ", ring ");
            say_dec(&line, iv.ring_id);
            say_text(&line, c->irq < 0 ? "; no MSI to hear" : heard ? "; its MSI heard" : "; its MSI NOT HEARD");
        } else {
            say_text(&line, "'s trap did NOT reach the IH; wptr ");
            say_hex(&line, rd(c, AMDGPU_IH_RB_WPTR), 5);
            say_text(&line, ", IH_RB_CNTL ");
            say_hex(&line, rd(c, AMDGPU_IH_RB_CNTL), 8);
            say_text(&line, ", IH_CNTL ");
            say_hex(&line, rd(c, AMDGPU_IH_CNTL), 8);
        }
        say_send(console, &line);
    }
}

/*--------------------------------------------------------------------------
 * **The GART and programs' address spaces** (`docs/amdgpu.md` V2), asked
 * for with GPU_OP_ENGINE_TEST `x` 2 - never at boot, since it turns the
 * memory controller's translation on under a screen the firmware drew.
 *
 * SYS_PAGES pages of system memory, one physical run. The GART spans
 * GTT_BYTES where `radeon_gtt_location` puts it; every entry points at
 * page 4, the dummy, and GTT pages 0 and 1 at pages 0 and 2 - so a write
 * that lands where it should proves the table is read rather than the
 * address taken as it is. Then VMIDs 1 and 2, a page directory and a table
 * each in VRAM, the same address in both on a page of its own - 1 and 5 -
 * and the same page of indirect buffers, 3; DMA 0 runs one buffer in each,
 * and a third in VMID 1 at an address neither maps, which must fault.
 *------------------------------------------------------------------------*/

#define SYS_GTT0     0u     /* GTT page 0 */
#define SYS_VMID1    1u     /* VA_TARGET in VMID 1 */
#define SYS_GTT1     2u     /* GTT page 1 */
#define SYS_IBS      3u     /* VA_IBS in both */
#define SYS_DUMMY    4u     /* every other GTT page, and where faults go */
#define SYS_VMID2    5u     /* VA_TARGET in VMID 2 */

static uint32_t *sys_word(const struct card *c, unsigned page, uint32_t offset)
{
    return (uint32_t *)(void *)(c->sys + page * 4096u + offset);
}

static void vram_wr64(const struct card *c, uint32_t offset, uint64_t value)
{
    vram_wr(c, offset, (uint32_t)value);
    vram_wr(c, offset + 4u, (uint32_t)(value >> 32));
}

static bool sys_pages(long console, unsigned i)
{
    struct card *c = &CARD[i];
    struct say_line line;
    long region, at, bus;

    if (c->sys != NULL) {
        return true;
    }

    region = kosmos_mem_create_flags(SYS_PAGES, MEM_CONTIGUOUS);
    if (region < 0 || (at = kosmos_mem_map(region)) < 0 || (bus = kosmos_mem_phys(region)) < 0) {
        begin(&line, i);
        say_text(&line, "no system pages for the GART test");
        say_send(console, &line);
        return false;
    }

    c->sys = (uint8_t *)(uintptr_t)at;
    c->sys_bus = (uint64_t)bus;
    return true;
}

/* The GART on: placed, its table filled, the aperture and the program. */
static bool gart_on(long console, unsigned i)
{
    struct card *c = &CARD[i];
    struct say_line line;
    struct amdgpu_gart_cfg cfg;
    struct amdgpu_prog p;
    uint64_t size = GTT_BYTES, dummy;
    uint32_t k, entries;

    if (c->gart_on) {
        return true;
    }

    amdgpu_gtt_place(c->fb_start, c->fb_end, (1ull << 40) - 1u, &size, &c->gtt_start, &c->gtt_end);
    entries = (uint32_t)(size / 4096u);

    if (size != GTT_BYTES || !amdgpu_vm_geometry(VM_GIGABYTES, &c->vm)) {
        begin(&line, i);
        say_text(&line, "no room for the GTT beside VRAM");
        say_send(console, &line);
        return false;
    }

    /* Every entry the dummy page, valid - where radeon leaves an unbound
     * entry invalid and lets a fault default there, which context 0 is not
     * told to do for anything but its range. */
    dummy = amdgpu_pte(c->sys_bus + SYS_DUMMY * 4096u, AMDGPU_PTE_SYSTEM_RW);
    for (k = 0; k < entries; k++) {
        vram_wr64(c, WORK_GART + k * 8u, dummy);
    }
    vram_wr64(c, WORK_GART + 0u, amdgpu_pte(c->sys_bus + SYS_GTT0 * 4096u, AMDGPU_PTE_SYSTEM_RW));
    vram_wr64(c, WORK_GART + 8u, amdgpu_pte(c->sys_bus + SYS_GTT1 * 4096u, AMDGPU_PTE_SYSTEM_RW));
    hdp_flush(c);

    /* VRAM untranslated under NOT_IN_SYS: the aperture is VRAM. */
    amdgpu_prog_clear(&p);
    amdgpu_sys_aperture(&p, c->fb_start, c->fb_end, work_mc(c, WORK_IH_DUMMY),
                        rd(c, AMDGPU_MC_VM_SYS_APER_LOW), rd(c, AMDGPU_MC_VM_SYS_APER_HIGH));
    if (p.n != 0) {
        begin(&line, i);
        say_text(&line, "the system aperture set to VRAM, as `si_mc_program` sets it");
        say_send(console, &line);
        (void)run(c, &p);
    }

    cfg.table_mc = work_mc(c, WORK_GART);
    cfg.gtt_start = c->gtt_start;
    cfg.gtt_end = c->gtt_end;
    cfg.dummy_page = c->sys_bus + SYS_DUMMY * 4096u;
    cfg.vm = &c->vm;

    if (!amdgpu_gart_enable(&p, &cfg) || run(c, &p) >= 0) {
        begin(&line, i);
        say_text(&line, "the GART NOT enabled: its program was refused");
        say_send(console, &line);
        return false;
    }

    c->gart_on = true;
    c->sys_low = c->fb_start;
    c->sys_high = c->fb_end;
    c->sys_set = true;

    begin(&line, i);
    say_text(&line, "GART on: ");
    say_mb(&line, size);
    say_text(&line, " at MC ");
    say_hex(&line, (unsigned long)c->gtt_start, 10);
    say_text(&line, ", its table at MC ");
    say_hex(&line, (unsigned long)cfg.table_mc, 10);
    say_text(&line, "; programs' contexts ");
    say_dec(&line, c->vm.gigabytes);
    say_text(&line, " GB, block ");
    say_dec(&line, c->vm.block_size);
    say_send(console, &line);
    return true;
}

/* A program's page directory and its one table: VA_TARGET on `target`,
 * VA_IBS on SYS_IBS, nothing else. */
static void vm_tables(const struct card *c, uint32_t pd, uint32_t pt, unsigned target)
{
    uint32_t de, te, k;

    for (k = 0; k < c->vm.pd_entries * 8u; k += 4) {
        vram_wr(c, pd + k, 0);
    }
    for (k = 0; k < c->vm.pt_entries * 8u; k += 4) {
        vram_wr(c, pt + k, 0);
    }

    (void)amdgpu_vm_index(&c->vm, VA_TARGET, &de, &te);
    vram_wr64(c, pd + de * 8u, amdgpu_pde(work_mc(c, pt)));
    vram_wr64(c, pt + te * 8u, amdgpu_pte(c->sys_bus + target * 4096u, AMDGPU_PTE_SYSTEM_RW));
    (void)amdgpu_vm_index(&c->vm, VA_IBS, &de, &te);
    vram_wr64(c, pt + te * 8u, amdgpu_pte(c->sys_bus + SYS_IBS * 4096u, AMDGPU_PTE_SYSTEM_RW));
    hdp_flush(c);
}

/* An indirect buffer of eight: one WRITE, padded with NOPs. */
static void ib_write(const struct card *c, uint32_t offset, uint64_t va, uint32_t value)
{
    uint32_t *ib = sys_word(c, SYS_IBS, offset);
    unsigned n = amdgpu_dma_write(ib, va, value);

    while (n < 8u) {
        n += amdgpu_dma_nop(ib + n);
    }
}

static void gart_test(long console, unsigned i, struct gpu_reply *rep)
{
    struct card *c = &CARD[i];
    struct say_line line;
    struct amdgpu_iv iv;
    struct amdgpu_vm_fault f;
    uint32_t cmd[128], n = 0, k, status, addr;
    unsigned long t0, t1 = 0;
    long command = config(i, PCI_COMMAND_WORD);
    bool fenced, faulted, heard, gtt_ok, vm_ok;
    uint32_t got[6];

    rep->error = GPU_ERR_NO_RING;

    if (!c->posted || !c->work_ok || !c->untranslated || !c->ih_on) {
        begin(&line, i);
        say_text(&line, "no GART test: ");
        say_text(&line, !c->posted ? "the card is not posted" : !c->work_ok ? "no work area"
                        : !c->untranslated ? "its addresses may be translated" : "no interrupt ring");
        say_send(console, &line);
        return;
    }

    if (command < 0 || ((uint32_t)command & 0x4u) == 0) {
        begin(&line, i);
        say_text(&line, "no GART test: bus master is off, so the card cannot reach system memory");
        say_send(console, &line);
        return;
    }

    if ((rd(c, AMDGPU_DMA_RB_CNTL) & AMDGPU_DMA_RB_ENABLE) || !sys_pages(console, i) || !gart_on(console, i)) {
        rep->error = GPU_ERR_NO_MEMORY;
        return;
    }

    memset(&iv, 0, sizeof iv);
    memset(c->sys, 0, SYS_PAGES * 4096u);
    vm_tables(c, WORK_PD1, WORK_PT1, SYS_VMID1);
    vm_tables(c, WORK_PD2, WORK_PT2, SYS_VMID2);
    ib_write(c, 0x000u, VA_TARGET, 0x11111111u);
    ib_write(c, 0x100u, VA_TARGET, 0x22222222u);
    ib_write(c, 0x200u, VA_UNMAPPED + 0x800u, 0x33333333u);   /* off word 0, if it lands in the dummy */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);         /* the buffers before the engine is told */

    vram_wr(c, WORK_WORDS + 8u, 0);
    (void)ih_drain(console, i, ~0u, NULL, 0);
    wr(c, AMDGPU_VM_CONTEXT1_CNTL2, rd(c, AMDGPU_VM_CONTEXT1_CNTL2) | 1u);   /* any old fault let go */

    /* V2a: through the GART, three pages: 0 and 1 mapped, 2 the dummy's. */
    n += amdgpu_dma_write(cmd + n, c->gtt_start + 0x000u, 0x6a770000u);
    n += amdgpu_dma_write(cmd + n, c->gtt_start + 0x1000u, 0x6a770001u);
    n += amdgpu_dma_write(cmd + n, c->gtt_start + 0x2000u, 0x6a770002u);

    /* V2b: VMID 1, VMID 2, and VMID 1 again at an address it does not map. */
    n += amdgpu_dma_vm_flush(cmd + n, 1, work_mc(c, WORK_PD1));
    n += amdgpu_dma_ib(cmd + n, n, 1, VA_IBS + 0x000u, 8);
    n += amdgpu_dma_vm_flush(cmd + n, 2, work_mc(c, WORK_PD2));
    n += amdgpu_dma_ib(cmd + n, n, 2, VA_IBS + 0x100u, 8);
    n += amdgpu_dma_ib(cmd + n, n, 1, VA_IBS + 0x200u, 8);

    n += amdgpu_dma_fence(cmd + n, work_mc(c, WORK_WORDS + 8u), 2);
    n += amdgpu_dma_trap(cmd + n);
    while ((n & 7u) != 0) {
        n += amdgpu_dma_nop(cmd + n);
    }

    dma_fill_ring(c, WORK_RING0, cmd, n);
    t0 = kosmos_ticks();
    dma_start(c, 0, WORK_RING0, WORK_WORDS, n);
    fenced = word_wait(c, WORK_WORDS + 8u, 2u, &t1);
    faulted = ih_wait(console, i, AMDGPU_SRC_VM_FAULT, 10, &iv, &heard)
              || ih_drain(console, i, AMDGPU_SRC_VM_FAULT2, &iv, 4);
    status = rd(c, AMDGPU_VM_CONTEXT1_FAULT_STATUS);
    addr = rd(c, AMDGPU_VM_CONTEXT1_FAULT_ADDR);
    wr(c, AMDGPU_VM_CONTEXT1_CNTL2, rd(c, AMDGPU_VM_CONTEXT1_CNTL2) | 1u);
    dma_stop(c, 0);

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    got[0] = *sys_word(c, SYS_GTT0, 0);
    got[1] = *sys_word(c, SYS_GTT1, 0);
    got[2] = *sys_word(c, SYS_DUMMY, 0);
    got[3] = *sys_word(c, SYS_VMID1, 0);
    got[4] = *sys_word(c, SYS_VMID2, 0);
    got[5] = *sys_word(c, SYS_DUMMY, 0x800u);
    amdgpu_vm_fault_decode(status, addr, &f);

    gtt_ok = got[0] == 0x6a770000u && got[1] == 0x6a770001u && got[2] == 0x6a770002u;
    vm_ok = got[3] == 0x11111111u && got[4] == 0x22222222u;
    /* Nothing else written: every word but the five targets' first, and
     * the page of buffers, still nought. */
    for (k = 0; k < SYS_PAGES * 1024u; k++) {
        bool target = k == SYS_GTT0 * 1024u || k == SYS_GTT1 * 1024u || k == SYS_DUMMY * 1024u
                      || k == SYS_VMID1 * 1024u || k == SYS_VMID2 * 1024u || k / 1024u == SYS_IBS
                      || k == SYS_DUMMY * 1024u + 0x200u;      /* the fault's write, if it lands */

        if (!target && ((const uint32_t *)(const void *)c->sys)[k] != 0) {
            gtt_ok = vm_ok = false;                 /* a write landed where nothing maps */
        }
    }

    begin(&line, i);
    say_text(&line, gtt_ok ? "GART works: " : "GART FAILED: ");
    say_text(&line, "GTT pages 0, 1, 2 wrote ");
    say_hex(&line, got[0], 8);
    say_text(&line, " ");
    say_hex(&line, got[1], 8);
    say_text(&line, " ");
    say_hex(&line, got[2], 8);
    say_text(&line, " to system pages 0, 2 and the dummy; fence ");
    say_text(&line, fenced ? "reached, " : "NOT REACHED, ");
    say_dec(&line, t1 ? counter_us(t0, t1) : 0u);
    say_text(&line, " us");
    say_send(console, &line);

    begin(&line, i);
    say_text(&line, vm_ok ? "VMIDs work: " : "VMIDs FAILED: ");
    say_text(&line, "the same address wrote ");
    say_hex(&line, got[3], 8);
    say_text(&line, " in VMID 1 and ");
    say_hex(&line, got[4], 8);
    say_text(&line, " in VMID 2, each to its own page");
    say_send(console, &line);

    begin(&line, i);
    faulted = faulted && f.vmid == 1u && f.address == VA_UNMAPPED;
    if (faulted) {
        say_text(&line, "the control faulted as it should: source ");
    } else {
        say_text(&line, iv.src_id == AMDGPU_SRC_VM_FAULT || iv.src_id == AMDGPU_SRC_VM_FAULT2
                        ? "the control faulted ELSEWHERE: source " : "the control did NOT fault: source ");
    }
    say_dec(&line, faulted ? iv.src_id : 0u);
    say_text(&line, ", VMID ");
    say_dec(&line, f.vmid);
    say_text(&line, f.write ? " writing " : " reading ");
    say_hex(&line, (unsigned long)f.address, 10);
    say_text(&line, ", client ");
    say_hex(&line, f.client, 2);
    say_text(&line, ", protections ");
    say_hex(&line, f.protections, 1);
    say_text(&line, got[5] == 0x33333333u ? "; its write went to the dummy page" : "; its write went nowhere seen");
    say_text(&line, c->irq < 0 ? "" : heard ? "; MSI heard" : "; MSI not heard");
    say_send(console, &line);

    rep->error = !fenced ? GPU_ERR_NO_ANSWER : gtt_ok && vm_ok && faulted ? GPU_OK : GPU_ERR_UNCHANGED;
    rep->engine.value = got[0];
    rep->engine.head = status;
    rep->engine.tail = addr;
    rep->engine.ctl = got[3];
    rep->engine.mode = got[4];
    rep->engine.esr = faulted ? iv.src_id : 0u;
    rep->engine.elapsed_us = t1 ? (uint32_t)counter_us(t0, t1) : 0u;
}

/*--------------------------------------------------------------------------
 * **The command processor** (`docs/amdgpu.md` V3), asked for with
 * GPU_OP_ENGINE_TEST `x` 3: the RLC and the CP's three given their
 * microcode, the ring 0 set up, ME_INITIALIZE, the ring test Linux makes -
 * SCRATCH_REG0 written by the CP - and an end-of-pipe fence with its
 * interrupt. The clear state (`si_cs_data`) is V5's, so CLEAR_STATE is not
 * sent and nothing is drawn; the CP is halted again after, as the DMA test
 * leaves its engine.
 *------------------------------------------------------------------------*/

static bool load_fw(long console, unsigned i, enum amdgpu_fw_kind kind, struct amdgpu_fw *fw)
{
    static const char *const names[] = { "PFP", "ME", "CE", "RLC" };
    struct card *c = &CARD[i];
    struct say_line line;
    const uint8_t *bytes;
    size_t len;
    const char *why;

    if (!amdgpu_fw_image(c->family, kind, &bytes, &len)) {
        begin(&line, i);
        say_text(&line, "no CP test: the ");
        say_text(&line, names[kind]);
        say_text(&line, "'s firmware is not in this image");
        say_send(console, &line);
        return false;
    }

    why = amdgpu_fw_parse(kind, bytes, len, fw);
    if (why != NULL) {
        begin(&line, i);
        say_text(&line, "no CP test: the ");
        say_text(&line, names[kind]);
        say_text(&line, "'s firmware has ");
        say_text(&line, why);
        say_send(console, &line);
        return false;
    }

    return true;
}

static void ucode(const struct card *c, uint32_t addr_reg, uint32_t data_reg, const struct amdgpu_fw *fw)
{
    uint32_t k;

    wr(c, addr_reg, 0);
    for (k = 0; k < fw->count; k++) {
        wr(c, data_reg, amdgpu_fw_word(fw, k));
    }
    wr(c, addr_reg, 0);
}

static void cp_test(long console, unsigned i, struct gpu_reply *rep)
{
    struct card *c = &CARD[i];
    struct say_line line;
    struct amdgpu_fw pfp, me, ce, rlc;
    struct amdgpu_prog p;
    struct amdgpu_iv iv;
    uint32_t ring[64], n = 0, k, me_cntl, scratch, fence, cp_int;
    unsigned long t0, t1 = 0;
    bool ring_ok, fenced, eop = false, heard = false;
    int stop;

    rep->error = GPU_ERR_NO_RING;

    if (!c->posted || !c->work_ok || !c->untranslated || !c->ih_on) {
        begin(&line, i);
        say_text(&line, "no CP test: ");
        say_text(&line, !c->posted ? "the card is not posted" : !c->work_ok ? "no work area"
                        : !c->untranslated ? "its addresses may be translated" : "no interrupt ring");
        say_send(console, &line);
        return;
    }

    if (!load_fw(console, i, AMDGPU_FW_PFP, &pfp) || !load_fw(console, i, AMDGPU_FW_ME, &me)
        || !load_fw(console, i, AMDGPU_FW_CE, &ce) || !load_fw(console, i, AMDGPU_FW_RLC, &rlc)) {
        rep->error = GPU_ERR_NO_DEVICE;
        return;
    }

    me_cntl = rd(c, AMDGPU_CP_ME_CNTL);

    /* The RLC (`gfx_v6_0_rlc_resume`). */
    for (k = 0; k < 4096u; k += 4) {
        vram_wr(c, WORK_RLC_SR + k, 0);
        vram_wr(c, WORK_RLC_CS + k, 0);
    }
    hdp_flush(c);

    amdgpu_rlc_stop(&p, rd(c, AMDGPU_CP_INT_CNTL_RING0), rd(c, AMDGPU_GRBM_SOFT_RESET));
    if ((stop = run(c, &p)) >= 0) {
        begin(&line, i);
        say_text(&line, "CP test stopped: the RLC would not stop - waiting on register ");
        say_hex(&line, p.w[stop].value, 5);
        say_send(console, &line);
        return;
    }
    (void)amdgpu_rlc_prepare(&p, work_mc(c, WORK_RLC_SR), work_mc(c, WORK_RLC_CS));
    (void)run(c, &p);
    for (k = 0; k < rlc.count; k++) {
        wr(c, AMDGPU_RLC_UCODE_ADDR, k);
        wr(c, AMDGPU_RLC_UCODE_DATA, amdgpu_fw_word(&rlc, k));
    }
    wr(c, AMDGPU_RLC_UCODE_ADDR, 0);
    cp_int = rd(c, AMDGPU_CP_INT_CNTL_RING0);
    amdgpu_rlc_start(&p, cp_int);
    (void)run(c, &p);

    /* The CP's microcode (`gfx_v6_0_cp_gfx_load_microcode`). */
    amdgpu_cp_halt(&p);
    (void)run(c, &p);
    ucode(c, AMDGPU_CP_PFP_UCODE_ADDR, AMDGPU_CP_PFP_UCODE_DATA, &pfp);
    ucode(c, AMDGPU_CP_CE_UCODE_ADDR, AMDGPU_CP_CE_UCODE_DATA, &ce);
    ucode(c, AMDGPU_CP_ME_RAM_WADDR, AMDGPU_CP_ME_RAM_DATA, &me);
    amdgpu_cp_loaded(&p);
    (void)run(c, &p);

    /* Its ring (`gfx_v6_0_cp_gfx_resume`), then `_cp_gfx_start`'s first
     * packets committed while it is halted, and it is let go. */
    for (k = 0; k < GFX_RING_BYTES; k += 4) {
        vram_wr(c, WORK_GFX_RING + k, 0);
    }
    vram_wr(c, WORK_WORDS + 0xc0u, 0);
    if (!amdgpu_cp_resume(&p, work_mc(c, WORK_GFX_RING), GFX_RING_BYTES, work_mc(c, WORK_WORDS + 0xa0u))) {
        return;
    }
    (void)run(c, &p);

    n += amdgpu_pm4_me_init(ring + n, 8);
    for (k = 0; k < n; k++) {
        vram_wr(c, WORK_GFX_RING + k * 4u, ring[k]);
    }
    hdp_flush(c);
    wr(c, AMDGPU_CP_RB0_WPTR, n);
    amdgpu_cp_run(&p);
    (void)run(c, &p);

    /* The ring test (`gfx_v6_0_ring_test_ring`). */
    wr(c, AMDGPU_SCRATCH_REG0, 0xcafedeadu);
    k = n;
    n += amdgpu_pm4_ring_test(ring + n);
    for (; k < n; k++) {
        vram_wr(c, WORK_GFX_RING + k * 4u, ring[k]);
    }
    hdp_flush(c);
    t0 = kosmos_ticks();
    wr(c, AMDGPU_CP_RB0_WPTR, n);
    ring_ok = poll_reg(c, AMDGPU_SCRATCH_REG0, 0xffffffffu, 0xdeadbeefu, AMDGPU_POLL_US);
    t1 = kosmos_ticks();
    scratch = rd(c, AMDGPU_SCRATCH_REG0);

    /* A fence with its interrupt (`gfx_v6_0_ring_emit_fence`, and
     * TIME_STAMP_INT_ENABLE as `si_irq_set` turns it on). */
    (void)ih_drain(console, i, ~0u, NULL, 0);
    wr(c, AMDGPU_CP_INT_CNTL_RING0, rd(c, AMDGPU_CP_INT_CNTL_RING0) | AMDGPU_TIME_STAMP_INT_ENABLE);
    k = n;
    n += amdgpu_pm4_fence(ring + n, work_mc(c, WORK_WORDS + 0xc0u), 1, false, true);
    for (; k < n; k++) {
        vram_wr(c, WORK_GFX_RING + k * 4u, ring[k]);
    }
    hdp_flush(c);
    wr(c, AMDGPU_CP_RB0_WPTR, n);
    fenced = ring_ok && word_wait(c, WORK_WORDS + 0xc0u, 1u, &t1);
    fence = vram_rd(c, WORK_WORDS + 0xc0u);
    if (fenced) {
        eop = ih_wait(console, i, AMDGPU_SRC_CP_EOP, 10, &iv, &heard);
    }

    /* Halted again, its interrupt off. */
    wr(c, AMDGPU_CP_INT_CNTL_RING0, rd(c, AMDGPU_CP_INT_CNTL_RING0) & ~AMDGPU_TIME_STAMP_INT_ENABLE);
    amdgpu_cp_halt(&p);
    (void)run(c, &p);

    begin(&line, i);
    say_text(&line, "CP: microcode PFP ");
    say_hex(&line, pfp.ucode_version, 2);
    say_text(&line, ", ME ");
    say_hex(&line, me.ucode_version, 2);
    say_text(&line, ", CE ");
    say_hex(&line, ce.ucode_version, 2);
    say_text(&line, ", RLC ");
    say_hex(&line, rlc.ucode_version, 2);
    say_text(&line, " loaded (it was ");
    say_text(&line, (me_cntl & (AMDGPU_CP_ME_HALT | AMDGPU_CP_PFP_HALT)) ? "halted)" : "RUNNING)");
    say_send(console, &line);

    begin(&line, i);
    say_text(&line, ring_ok ? "CP ring works: SCRATCH_REG0 " : "CP ring FAILED: SCRATCH_REG0 ");
    say_hex(&line, scratch, 8);
    say_text(&line, ", rptr ");
    say_hex(&line, rd(c, AMDGPU_CP_RB0_RPTR), 4);
    say_text(&line, ", ");
    say_dec(&line, ring_ok ? counter_us(t0, t1) : 0u);
    say_text(&line, " us; EOP fence ");
    say_text(&line, fenced ? "reached" : "NOT REACHED");
    say_text(&line, eop ? ", source 181 in the IH" : ", NO source 181 in the IH");
    say_text(&line, c->irq < 0 ? "" : heard ? ", MSI heard" : ", MSI not heard");
    say_send(console, &line);

    rep->error = !ring_ok ? GPU_ERR_NO_ANSWER : fenced && eop ? GPU_OK : GPU_ERR_UNCHANGED;
    rep->engine.value = scratch;
    rep->engine.head = rd(c, AMDGPU_CP_RB0_RPTR);
    rep->engine.tail = n;
    rep->engine.ctl = rd(c, AMDGPU_CP_RB0_CNTL);
    rep->engine.mode = fence;
    rep->engine.esr = rd(c, AMDGPU_GRBM_STATUS);
    rep->engine.elapsed_us = ring_ok ? (uint32_t)counter_us(t0, t1) : 0u;
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

        /* `x` 2: the GART and VMIDs (V2); 3: the CP (V3). The first card
         * that answers either is the reply. */
        if (e >= 2u) {
            struct gpu_reply one;

            memset(&one, 0, sizeof one);
            if (e == 2u) {
                gart_test(console, i, &one);
            } else {
                cp_test(console, i, &one);
            }
            if (first || (rep->error != GPU_OK && one.error == GPU_OK)) {
                *rep = one;
                first = false;
            }
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

    /* V1: the interrupt ring, wherever the work area is VRAM untranslated. */
    for (i = 0; i < ncards; i++) {
        ih_start(console, i);
    }

    /* The DMA engines' first work, wherever it is safe to try (step 3a),
     * and their traps reaching the IH (V1). */
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
                engines(console, rq.x >= 0 && rq.x <= 3 ? (unsigned)rq.x : 0u, &rep);
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
