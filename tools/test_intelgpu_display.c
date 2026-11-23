/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `intelgpu_display.c` held to numbers, on the Mac (`docs/displaymodes.md`
 * D1): QEMU has no Intel GPU, so what the M700's firmware lit is read only
 * there. Each register below is built from the fields `intelgpu_display.h`
 * cites from i915, the way a firmware - or i915 itself - would have set
 * them for a real mode, and the pixel clock and refresh it should come to
 * were worked by hand from the mode's CVT reduced-blanking timings.
 *
 * Its control: `IGPUDISPLAYTEST_BROKEN=1` moves one field in each case
 * before decoding, and the run must fail.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/drivers/display/intelgpu_display.h"

static int checks, failed, broken;

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        failed++;
        printf("FAIL: %s\n", what);
    }
}

/* A timing register: the end or total in 31:16, the start or active in
 * 15:0, each less one. */
static uint32_t pair(unsigned high, unsigned low)
{
    return ((uint32_t)(high - 1u) << 16) | (low - 1u);
}

/*
 * **3440 x 1440 at 60 Hz over DisplayPort**, the M700's monitor as the
 * firmware probably lights it: HBR2, four lanes, 8 bits a colour,
 * transcoder A, DDI B on DPLL 1.
 *
 * CVT-RB, by hand (VESA CVT 1.1, as `drm_cvt_mode` computes it with
 * `reduced`): the horizontal blank is a fixed 160 - front porch 48, sync
 * 32, back porch 80 - so the total is 3600. The line period is
 * (1/60 s - 460 us) / 1440 = 11.2546 us, so 460 us of blanking is
 * 40.87 lines, and the blank is 41: front porch 3, sync 10 (21:9 is not
 * one of CVT's aspects), back porch 28 - a total of 1481. The pixel clock
 * is 60 x 1481 x 3600 = 319.896 MHz, rounded down to CVT's 0.25 MHz:
 * 319.75 MHz. The refresh it gives back is 319 750 000 / (3600 x 1481) =
 * 59.9726 Hz - 59 973 mHz.
 *
 * Link M/N as `intel_link_compute_m_n` writes it: N the constant 0x80000,
 * M = 319 750 x 0x80000 / 540 000 (HBR2's symbol clock) = 310 446.
 * `intel_dotclock_calculate` turns that back into 310 446 x 540 000 /
 * 0x80000 = 319 749.6, rounded up: 319 750.
 *
 * Data M/N, the same way: 319 750 kHz x 24 bits / 8 = 959 250 kB/s of
 * pixels over what four lanes carry, 540 000 x 10 x 4 x 0.8 / 8 =
 * 2 160 000 kB/s; N the constant 0x8000000, M = 959 250 x 0x8000000 /
 * 2 160 000 = 59 605 712, both halved four times to fit 24 bits:
 * 3 725 357 over 0x800000. The transfer unit is 64.
 */
static void test_dp_3440(void)
{
    struct igpu_output_regs r;
    struct igpu_output o;

    memset(&r, 0, sizeof r);
    r.ddi_func_ctl = (1u << 31) | (1u << 28) | (2u << 24) | (0u << 20) |
                     (1u << 16) | ((4u - 1u) << 1);     /* +H, -V: CVT-RB's */
    r.timing.htotal = pair(3600, 3440);
    r.timing.hblank = pair(3600, 3440);
    r.timing.hsync = pair(3520, 3488);
    r.timing.vtotal = pair(1481, 1440);
    r.timing.vblank = pair(1481, 1440);
    r.timing.vsync = pair(1453, 1443);
    r.data_m1 = ((64u - 1u) << 25) | 3725357u;
    r.data_n1 = 0x800000u;
    r.link_m1 = 310446u;
    r.link_n1 = 0x80000u;
    r.ddi_buf_ctl = (1u << 31) | ((4u - 1u) << 1);
    /* DPLL 0: override, 810 (code 2) - the display clock's VCO of 8.1 GHz.
     * DPLL 1: override, 2700 (code 0). DPLL 2: override, HDMI mode. */
    r.dpll_ctrl1 = (1u | (2u << 1)) | ((1u | (0u << 1)) << 6) | ((1u | (1u << 5)) << 12);
    /* DDI B (port 1) takes DPLL 1: override in bit 3, the DPLL in 5:4;
     * DDI C (port 2) takes DPLL 2: override 6, DPLL 8:7. Every other
     * DDI's clock is off, bits 15 + port. */
    r.dpll_ctrl2 = (1u << 3) | (1u << 4) | (1u << 6) | (2u << 7) |
                   (1u << 15) | (1u << 18) | (1u << 19);

    if (broken) {
        r.link_n1 = 0x40000u;               /* half of N: twice the clock */
    }

    igpu_decode_output(&r, &o);

    check(o.func.enabled, "DP: the transcoder's DDI function is on");
    check(o.func.port == 1 && strcmp(igpu_port_name(o.func.port), "DDI B") == 0,
          "DP: DDI B from bits 30:28");
    check(o.is_dp && o.func.mode_name && strcmp(o.func.mode_name, "DisplayPort") == 0,
          "DP: mode select 2 is DisplayPort SST");
    check(o.func.bpc == 8, "DP: bits a colour code 0 is 8");
    check(o.func.lanes == 4, "DP: four lanes from 3:1 plus one");
    check(o.func.hsync_positive && !o.func.vsync_positive, "DP: +hsync, -vsync");
    check(o.func.edp_pipe == -1, "DP: not the eDP transcoder, so no eDP input");

    check(o.t.hactive == 3440 && o.t.vactive == 1440, "DP: 3440 x 1440 active, each one more than written");
    check(o.t.htotal == 3600 && o.t.vtotal == 1481, "DP: 3600 x 1481 in all");
    check(o.t.hsync_start == 3488 && o.t.hsync_end == 3520, "DP: hsync 3488-3520");
    check(o.t.vsync_start == 1443 && o.t.vsync_end == 1453, "DP: vsync 1443-1453");
    check(o.t.hblank_start == 3440 && o.t.vblank_end == 1481, "DP: blanks from the active edge to the total");

    check(o.mn.tu == 64, "DP: transfer unit 64 from 30:25 plus one");
    check(o.mn.data_m == 3725357u && o.mn.data_n == 0x800000u, "DP: data M/N in 23:0");

    check(o.buf.enabled && o.buf.lanes == 4 && !o.buf.idle, "DP: DDI buffer on, four lanes, not idle");

    check(o.dpll_known && o.dpll == 1 && !o.dpll_clock_off, "DP: DDI B takes DPLL 1, its clock on");
    check(o.pll.override && !o.pll.hdmi && !o.pll.ssc && o.pll.rate_code == 0,
          "DP: DPLL 1 overridden, DP mode, rate code 0");
    check(o.port_clock == 540000, "DP: rate 2700 is a 540 000 port clock, HBR2");

    check(o.pixel_khz == 319750, "DP: pixel clock 319 750 kHz from link M/N");
    check(o.refresh_mhz == 59973, "DP: 59 973 mHz");
    check(o.bpp_milli == 24000, "DP: data M/N says 24 bits a pixel");

    if (o.pixel_khz != 319750 || o.refresh_mhz != 59973) {
        printf("      DP: got %u kHz, %u mHz\n", o.pixel_khz, o.refresh_mhz);
    }
}

/*
 * **1920 x 1080 at 60 Hz over HDMI**: transcoder B, DDI C on DPLL 2 in HDMI
 * mode, 8 bits a colour.
 *
 * CVT-RB, by hand: total 2080 (1920 + 160: 48, 32, 80); the line period is
 * (1/60 s - 460 us) / 1080 = 15.0062 us, so the blank is 30.65 + 1 = 31
 * lines - 3, sync 5 (16:9), back 23 - a total of 1111; the clock is
 * 60 x 1111 x 2080 = 138.653 MHz, down to 0.25: 138.5 MHz.
 *
 * The WRPLL for it, by `skl_ddi_calculate_wrpll`'s search worked by hand:
 * the AFE clock is five times the pixel clock, 692.5 MHz; the even divider
 * with the least deviation is 14, a DCO of 9695 MHz, +0.99% of the 9.6 GHz
 * centre (12 gives 8310 MHz, -1.07% of 8.4 GHz). 14 is P 7, Q 1, K 2
 * (`skl_wrpll_get_multipliers`); the DCO over 24 MHz is 403 and
 * 23/24, a fraction of 31 402 over 0x8000. Read back as
 * `skl_ddi_wrpll_get_freq` does - 403 x 24 000 + 31 402 x 24 000 / 0x8000
 * = 9 694 999 kHz, over 7 x 1 x 2 x 5 - it is 138 499 kHz, a kilohertz
 * short, exactly as i915 would read it. The refresh is
 * 138 499 000 / (2080 x 1111) = 59.9334 Hz.
 */
static void test_hdmi_1080(void)
{
    struct igpu_output_regs r;
    struct igpu_output o;

    memset(&r, 0, sizeof r);
    r.ddi_func_ctl = (1u << 31) | (2u << 28) | (0u << 24) | (0u << 20) | (1u << 16) |
                     (3u << 1);         /* a stale width the decoder must ignore for HDMI */
    r.timing.htotal = pair(2080, 1920);
    r.timing.hblank = pair(2080, 1920);
    r.timing.hsync = pair(2000, 1968);
    r.timing.vtotal = pair(1111, 1080);
    r.timing.vblank = pair(1111, 1080);
    r.timing.vsync = pair(1088, 1083);
    r.ddi_buf_ctl = (1u << 31);
    r.dpll_ctrl1 = (1u | (2u << 1)) | ((1u | (1u << 5)) << 12);
    r.dpll_ctrl2 = (1u << 6) | (2u << 7);
    /* CFGCR1: FREQ_ENABLE 31, fraction 23:9, integer 8:0. CFGCR2: Q ratio
     * 15:8 with Q mode 7 clear (Q is 1), K 6:5 = 1 (2), P 4:2 = 4 (7),
     * centre 1:0 = 0 (9.6 GHz). */
    r.cfgcr1 = (1u << 31) | (31402u << 9) | 403u;
    r.cfgcr2 = (1u << 8) | (1u << 5) | (4u << 2) | 0u;

    if (broken) {
        r.cfgcr2 = (1u << 8) | (1u << 5) | (2u << 2);   /* P 3: a faster clock */
    }

    igpu_decode_output(&r, &o);

    check(o.func.port == 2 && o.func.mode == IGPU_DDI_HDMI && !o.is_dp, "HDMI: DDI C, mode 0");
    check(o.func.mode_name && strcmp(o.func.mode_name, "HDMI") == 0, "HDMI: named");
    check(o.func.lanes == 4, "HDMI: four lanes whatever 3:1 says, below version 14");
    check(o.func.hsync_positive && !o.func.vsync_positive, "HDMI: +hsync, -vsync");
    check(o.t.hactive == 1920 && o.t.vactive == 1080 && o.t.htotal == 2080 && o.t.vtotal == 1111,
          "HDMI: 1920 x 1080 of 2080 x 1111");
    check(o.dpll_known && o.dpll == 2, "HDMI: DDI C takes DPLL 2");
    check(o.pll.hdmi && o.pll.port_clock == 0, "HDMI: DPLL 2 in HDMI mode, no link rate");
    check(o.port_clock == 138499, "HDMI: the WRPLL comes to 138 499 kHz");
    check(o.pixel_khz == 138499, "HDMI: at 8 bits a colour the pixel clock is the TMDS clock");
    check(o.refresh_mhz == 59933, "HDMI: 59 933 mHz");
    check(o.bpp_milli == 0, "HDMI: no data M/N");

    if (o.port_clock != 138499 || o.refresh_mhz != 59933) {
        printf("      HDMI: got %u kHz, %u mHz\n", o.port_clock, o.refresh_mhz);
    }

    /* 1080p at 148.5 MHz (CTA's VIC 16): the search gives 12, P 2, Q 3, K 2,
     * the 9 GHz centre, a DCO of 8910 MHz - 371 and 8192 / 0x8000 - which
     * reads back exact. Q 3 is in 15:8 with the mode bit set. */
    check(igpu_wrpll_khz((1u << 31) | (8192u << 9) | 371u,
                         (3u << 8) | (1u << 7) | (1u << 5) | (1u << 2) | 1u,
                         IGPU_REF_KHZ) == 148500,
          "WRPLL: 148 500 kHz with Q used");
    check(igpu_wrpll_khz((1u << 31) | (8192u << 9) | 371u,
                         (3u << 8) | (1u << 5) | (1u << 2) | 1u,
                         IGPU_REF_KHZ) == 445500,
          "WRPLL: Q mode clear ignores the Q ratio");
    check(igpu_wrpll_khz(371u, (3u << 2), IGPU_REF_KHZ) == 0, "WRPLL: P code 3 is not a divider");
    check(igpu_wrpll_khz(371u, (5u << 2) | (3u << 5), IGPU_REF_KHZ) == 24000u * 371u / 35u,
          "WRPLL: P code 5 is read as 7, K code 3 as 1");

    /* Deep colour over HDMI: the TMDS clock is 1.25 times the pixel's. */
    check(igpu_hdmi_pixel_khz(185625, 10) == 148500, "HDMI: 10 bits a colour, 24/30 of the TMDS clock");
    check(igpu_hdmi_pixel_khz(222750, 12) == 148500, "HDMI: 12 bits a colour, 24/36");
}

/* The eDP transcoder's pipe, and the codes around the fields. */
static void test_fields(void)
{
    struct igpu_ddi_func f;
    struct igpu_dpll p;
    struct igpu_ddi_buf b;
    struct igpu_m_n mn;
    unsigned id;
    bool off;

    igpu_decode_ddi_func((1u << 31) | (2u << 24) | (6u << 12), true, &f);
    check(f.edp_pipe == 2, "eDP input 6 (C on-off) is pipe C");
    igpu_decode_ddi_func((1u << 31) | (2u << 24) | (4u << 12), true, &f);
    check(f.edp_pipe == 0, "eDP input 4 (A on-off) is pipe A");
    igpu_decode_ddi_func((1u << 31) | (2u << 24) | (0u << 12), true, &f);
    check(f.edp_pipe == 0, "eDP input 0 (A on) is pipe A");
    igpu_decode_ddi_func((1u << 31) | (2u << 24) | (5u << 12), true, &f);
    check(f.edp_pipe == 1, "eDP input 5 is pipe B");

    igpu_decode_ddi_func((1u << 20), false, &f);
    check(!f.enabled && f.bpc == 10, "bits a colour code 1 is 10, and bit 31 clear is off");
    igpu_decode_ddi_func((2u << 20), false, &f);
    check(f.bpc == 6, "code 2 is 6");
    igpu_decode_ddi_func((3u << 20), false, &f);
    check(f.bpc == 12, "code 3 is 12");
    igpu_decode_ddi_func((5u << 20) | (7u << 24) | ((2u - 1u) << 1), false, &f);
    check(f.bpc == 0 && f.mode_name == NULL && f.lanes == 2, "codes i915 does not name say so; DP width 2");
    igpu_decode_ddi_func((3u << 24), false, &f);
    check(f.mode_name && strcmp(f.mode_name, "DP MST") == 0, "mode 3 is DP MST");
    igpu_decode_ddi_func((1u << 24) | (4u << 28), false, &f);
    check(f.mode == IGPU_DDI_DVI && f.port == 4 && f.lanes == 4, "DVI on DDI E, four lanes");

    /* Every link rate, each DPLL's six bits. */
    {
        static const struct { unsigned code, port_clock; } rates[] = {
            { 0, 540000 }, { 1, 270000 }, { 2, 162000 },
            { 3, 324000 }, { 4, 216000 }, { 5, 432000 },
        };
        unsigned i, dpll;
        int all = 1;

        for (dpll = 0; dpll < IGPU_DPLLS; dpll++) {
            for (i = 0; i < 6; i++) {
                igpu_decode_dpll_ctrl1((1u | (rates[i].code << 1)) << (dpll * 6u), dpll, &p);
                all &= p.override && p.rate_code == rates[i].code && p.port_clock == rates[i].port_clock;
            }
        }

        check(all, "DPLL_CTRL1: six rates on four DPLLs, six bits apart");
    }

    igpu_decode_dpll_ctrl1((1u << 4) << 6, 1, &p);
    check(p.ssc && !p.override, "DPLL_CTRL1: SSC is bit 4 of a DPLL's six");
    igpu_decode_dpll_ctrl1(1u | (6u << 1), 0, &p);
    check(p.port_clock == 0, "DPLL_CTRL1: rate codes 6 and 7 are not named");

    check(!igpu_decode_dpll_ctrl2(2u << 1, 0, &id, &off), "DPLL_CTRL2: no override, no DPLL");
    check(igpu_decode_dpll_ctrl2(1u | (3u << 1), 0, &id, &off) && id == 3 && !off,
          "DPLL_CTRL2: DDI A on DPLL 3");
    check(igpu_decode_dpll_ctrl2((1u << 12) | (1u << 13) | (1u << 19), 4, &id, &off) && id == 1 && off,
          "DPLL_CTRL2: DDI E on DPLL 1, clock off in bit 19");

    igpu_decode_ddi_buf((1u << 31) | (9u << 24) | (1u << 16) | (1u << 7) | (1u << 4) | (1u << 1) | 1u, &b);
    check(b.enabled && b.trans_select == 9 && b.reversed && b.idle && b.a_four_lanes &&
          b.lanes == 2 && b.detected, "DDI_BUF_CTL: every field");

    igpu_decode_m_n((1u << 31) | (5u << 25) | (1u << 24) | 0x123456u, 0xFF800000u, 0x01ABCDEFu, 0x00080000u, &mn);
    check(mn.tu == 6 && mn.data_m == 0x123456u && mn.data_n == 0x800000u && mn.link_m == 0xABCDEFu,
          "M/N: 23:0 each, TU in 30:25 plus one");
    mn.link_n = 0;
    check(igpu_dp_pixel_khz(&mn, 540000) == 0, "no link N, no pixel clock");
    check(igpu_refresh_mhz(148500, 2200, 1125) == 60000, "VIC 16 is 60.000 Hz");
    check(igpu_refresh_mhz(148500, 0, 1125) == 0, "no total, no refresh");

    check(igpu_ddi_buf_ctl_at(2) == 0x64200u, "DDI C's buffer at 0x64200");
    check(igpu_dpll_cfgcr1_at(1) == 0x6c040u && igpu_dpll_cfgcr1_at(3) == 0x6c050u &&
          igpu_dpll_cfgcr2_at(2) == 0x6c04cu && igpu_dpll_cfgcr1_at(0) == 0,
          "CFGCR offsets: DPLL 1 to 3, 8 apart, none for DPLL 0");
    check(igpu_transcoder_at(3) + IGPU_TRANS_DDI_FUNC_CTL == 0x6f400u,
          "the eDP transcoder's TRANS_DDI_FUNC_CTL at 0x6F400");
    check(igpu_transcoder_at(1) + IGPU_TRANS_LINK_N1 == 0x61044u, "transcoder B's LINK_N1 at 0x61044");
}

/* The display clock, and how fast it may go. */
static void test_cdclk(void)
{
    struct igpu_cdclk c;
    uint32_t on = (1u << 31) | (1u << 30);
    uint32_t dpll0_810 = 1u | (2u << 1), dpll0_2160 = 1u | (5u << 1);

    /* 450 MHz on an 8.1 GHz VCO, its decimal (450 000 - 1000) / 500 = 898. */
    igpu_decode_cdclk(((broken ? 1u : 0u) << 26) | 898u, on, dpll0_810, &c);
    check(c.pll_on && c.vco_khz == 8100000 && c.cdclk_khz == 450000, "CDCLK: 450 MHz at 8.1 GHz");
    check(c.decimal_khz == 450000, "CDCLK: the decimal field agrees");

    igpu_decode_cdclk((3u << 26) | 1348u, on, dpll0_810, &c);
    check(c.cdclk_khz == 675000 && c.decimal_khz == 675000, "CDCLK: select 3 at 8.1 GHz is 675 MHz");
    igpu_decode_cdclk((2u << 26), on, dpll0_810, &c);
    check(c.cdclk_khz == 337500, "CDCLK: select 2 at 8.1 GHz is 337.5 MHz");
    igpu_decode_cdclk((3u << 26), on, dpll0_2160, &c);
    check(c.vco_khz == 8640000 && c.cdclk_khz == 617143, "CDCLK: select 3 at 8.64 GHz is 617.143 MHz");
    igpu_decode_cdclk((0u << 26), on, dpll0_2160, &c);
    check(c.cdclk_khz == 432000, "CDCLK: select 0 at 8.64 GHz is 432 MHz");
    igpu_decode_cdclk((1u << 26), on, dpll0_2160, &c);
    check(c.cdclk_khz == 540000, "CDCLK: select 1 is 540 MHz at either VCO");

    igpu_decode_cdclk((3u << 26), 1u << 31, dpll0_810, &c);
    check(!c.pll_on && c.vco_khz == 0 && c.cdclk_khz == 24000, "CDCLK: DPLL 0 not locked is the 24 MHz bypass");
    igpu_decode_cdclk((3u << 26), on, dpll0_810 | (1u << 4), &c);
    check(c.vco_khz == 0 && c.cdclk_khz == 24000, "CDCLK: DPLL 0 with SSC is no VCO i915 trusts");
    igpu_decode_cdclk((3u << 26), on, (2u << 1), &c);
    check(c.vco_khz == 0, "CDCLK: DPLL 0 without its override likewise");

    check(igpu_max_cdclk_khz(0u << 23, 8100000) == 675000, "max: fuse 675 at 8.1 GHz");
    check(igpu_max_cdclk_khz(0u << 23, 8640000) == 617143, "max: fuse 675 at 8.64 GHz is 617.143");
    check(igpu_max_cdclk_khz(1u << 23, 8100000) == 540000, "max: fuse 540");
    check(igpu_max_cdclk_khz(2u << 23, 8100000) == 450000, "max: fuse 450 at 8.1 GHz");
    check(igpu_max_cdclk_khz(2u << 23, 8640000) == 432000, "max: fuse 450 at 8.64 GHz is 432");
    check(igpu_max_cdclk_khz(3u << 23, 8100000) == 337500, "max: fuse 337.5");
    check(igpu_max_cdclk_khz((1u << 27) | (3u << 23) | (1u << 30), 8640000) == 308571,
          "max: the fuse's neighbours do not leak in");
}

int main(void)
{
    broken = getenv("IGPUDISPLAYTEST_BROKEN") != NULL;

    test_dp_3440();
    test_hdmi_1080();
    test_fields();
    test_cdclk();

    printf("%s: %d checks on intelgpu_display\n", failed ? "FAIL" : "PASS", checks);
    return failed != 0;
}
