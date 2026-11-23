/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A Gen9 display engine's output, read into what it means
 * (`intelgpu_display.h` has where every bit comes from).
 */

#include <stddef.h>

#include "intelgpu_display.h"

/* TRANS_DDI_FUNC_CTL (`i915_reg.h`). */
#define FUNC_ENABLE         (1u << 31)
#define FUNC_PORT_SHIFT     28
#define FUNC_PORT_MASK      0x7u
#define FUNC_MODE_SHIFT     24
#define FUNC_MODE_MASK      0x7u
#define FUNC_BPC_SHIFT      20
#define FUNC_BPC_MASK       0x7u
#define FUNC_PVSYNC         (1u << 17)
#define FUNC_PHSYNC         (1u << 16)
#define FUNC_EDP_SHIFT      12
#define FUNC_EDP_MASK       0x7u
#define FUNC_WIDTH_SHIFT    1
#define FUNC_WIDTH_MASK     0x7u

/* DATA_M1 and the rest (`intel_display_regs.h`). */
#define M_N_MASK            0x00FFFFFFu
#define TU_SHIFT            25
#define TU_MASK             0x3Fu

/* DPLL_CTRL1, DPLL_CTRL2 (`i915_reg.h`). */
#define CTRL1_OVERRIDE      (1u << 0)
#define CTRL1_RATE_SHIFT    1
#define CTRL1_RATE_MASK     0x7u
#define CTRL1_SSC           (1u << 4)
#define CTRL1_HDMI          (1u << 5)

/* DPLL_CFGCR1, DPLL_CFGCR2 (`i915_reg.h`). */
#define CFGCR1_INTEGER_MASK 0x1FFu
#define CFGCR1_FRACTION_SHIFT 9
#define CFGCR1_FRACTION_MASK 0x7FFFu
#define CFGCR2_QDIV_SHIFT   8
#define CFGCR2_QDIV_MASK    0xFFu
#define CFGCR2_QDIV_MODE    (1u << 7)
#define CFGCR2_KDIV_SHIFT   5
#define CFGCR2_KDIV_MASK    0x3u
#define CFGCR2_PDIV_SHIFT   2
#define CFGCR2_PDIV_MASK    0x7u

/* DDI_BUF_CTL (`i915_reg.h`). */
#define BUF_ENABLE          (1u << 31)
#define BUF_TRANS_SHIFT     24
#define BUF_TRANS_MASK      0xFu
#define BUF_REVERSAL        (1u << 16)
#define BUF_IDLE            (1u << 7)
#define BUF_A_4_LANES       (1u << 4)
#define BUF_WIDTH_SHIFT     1
#define BUF_WIDTH_MASK      0x7u
#define BUF_DETECTED        (1u << 0)

/* CDCLK_CTL, LCPLL1_CTL, SKL_DFSM (`i915_reg.h`; LCPLL_PLL_LOCK is the
 * Haswell name for bit 30, which `skl_dpll0_update` reads in LCPLL1_CTL). */
#define CDCLK_SEL_SHIFT     26
#define CDCLK_SEL_MASK      0x3u
#define CDCLK_DECIMAL_MASK  0x7FFu
#define LCPLL_ENABLE        (1u << 31)
#define LCPLL_LOCK          (1u << 30)
#define DFSM_LIMIT_SHIFT    23
#define DFSM_LIMIT_MASK     0x3u

#define VCO_8100            8100000u
#define VCO_8640            8640000u

uint32_t igpu_ddi_buf_ctl_at(unsigned port)
{
    return IGPU_DDI_BUF_CTL_A + port * IGPU_DDI_STRIDE;
}

uint32_t igpu_dpll_cfgcr1_at(unsigned dpll)
{
    if (dpll == 0 || dpll >= IGPU_DPLLS) {
        return 0;
    }

    return IGPU_DPLL1_CFGCR1 + (dpll - 1) * IGPU_DPLL_CFGCR_STRIDE;
}

uint32_t igpu_dpll_cfgcr2_at(unsigned dpll)
{
    if (dpll == 0 || dpll >= IGPU_DPLLS) {
        return 0;
    }

    return IGPU_DPLL1_CFGCR2 + (dpll - 1) * IGPU_DPLL_CFGCR_STRIDE;
}

uint32_t igpu_transcoder_at(unsigned transcoder)
{
    switch (transcoder) {
    case 0:  return IGPU_TRANSCODER_A;
    case 1:  return IGPU_TRANSCODER_B;
    case 2:  return IGPU_TRANSCODER_C;
    case 3:  return IGPU_TRANSCODER_EDP;
    default: return 0;
    }
}

const char *igpu_transcoder_name(unsigned transcoder)
{
    switch (transcoder) {
    case 0:  return "A";
    case 1:  return "B";
    case 2:  return "C";
    case 3:  return "eDP";
    default: return "?";
    }
}

const char *igpu_port_name(unsigned port)
{
    switch (port) {
    case 0:  return "DDI A";
    case 1:  return "DDI B";
    case 2:  return "DDI C";
    case 3:  return "DDI D";
    case 4:  return "DDI E";
    default: return "DDI ?";
    }
}

static const char *mode_name(unsigned mode)
{
    switch (mode) {
    case IGPU_DDI_HDMI:   return "HDMI";
    case IGPU_DDI_DVI:    return "DVI";
    case IGPU_DDI_DP_SST: return "DisplayPort";
    case IGPU_DDI_DP_MST: return "DP MST";
    case IGPU_DDI_FDI:    return "FDI";
    default:              return NULL;
    }
}

/* `TRANS_DDI_BPC_*`: the codes are not in order. */
static unsigned bpc_of(unsigned code)
{
    switch (code) {
    case 0:  return 8;
    case 1:  return 10;
    case 2:  return 6;
    case 3:  return 12;
    default: return 0;
    }
}

void igpu_decode_ddi_func(uint32_t v, bool edp_transcoder, struct igpu_ddi_func *out)
{
    unsigned mode = (v >> FUNC_MODE_SHIFT) & FUNC_MODE_MASK;

    out->enabled = (v & FUNC_ENABLE) != 0;
    out->port = (v >> FUNC_PORT_SHIFT) & FUNC_PORT_MASK;
    out->mode = mode;
    out->mode_name = mode_name(mode);
    out->bpc = bpc_of((v >> FUNC_BPC_SHIFT) & FUNC_BPC_MASK);
    out->hsync_positive = (v & FUNC_PHSYNC) != 0;
    out->vsync_positive = (v & FUNC_PVSYNC) != 0;

    /* `intel_ddi_read_func_ctl`: HDMI and DVI are four lanes below display
     * version 14, whatever the field says; DP is the field plus one. */
    if (mode == IGPU_DDI_HDMI || mode == IGPU_DDI_DVI) {
        out->lanes = 4;
    } else {
        out->lanes = ((v >> FUNC_WIDTH_SHIFT) & FUNC_WIDTH_MASK) + 1;
    }

    /* `hsw_enabled_transcoders`: 0 (A on) and 4 (A on-off) are both pipe A;
     * 5, 6 and 7 are B, C and D. 1-3 are unnamed, which i915 warns of and
     * takes as A. */
    out->edp_pipe = -1;

    if (edp_transcoder) {
        unsigned in = (v >> FUNC_EDP_SHIFT) & FUNC_EDP_MASK;

        out->edp_pipe = in >= 5 ? (int)(in - 4) : 0;
    }
}

void igpu_decode_timings(const struct igpu_timing_regs *r, struct igpu_timings *out)
{
    out->hactive = (r->htotal & 0xFFFFu) + 1;
    out->htotal = (r->htotal >> 16) + 1;
    out->hblank_start = (r->hblank & 0xFFFFu) + 1;
    out->hblank_end = (r->hblank >> 16) + 1;
    out->hsync_start = (r->hsync & 0xFFFFu) + 1;
    out->hsync_end = (r->hsync >> 16) + 1;
    out->vactive = (r->vtotal & 0xFFFFu) + 1;
    out->vtotal = (r->vtotal >> 16) + 1;
    out->vblank_start = (r->vblank & 0xFFFFu) + 1;
    out->vblank_end = (r->vblank >> 16) + 1;
    out->vsync_start = (r->vsync & 0xFFFFu) + 1;
    out->vsync_end = (r->vsync >> 16) + 1;
}

void igpu_decode_m_n(uint32_t data_m1, uint32_t data_n1, uint32_t link_m1,
                     uint32_t link_n1, struct igpu_m_n *out)
{
    out->tu = ((data_m1 >> TU_SHIFT) & TU_MASK) + 1;
    out->data_m = data_m1 & M_N_MASK;
    out->data_n = data_n1 & M_N_MASK;
    out->link_m = link_m1 & M_N_MASK;
    out->link_n = link_n1 & M_N_MASK;
}

/* `skl_ddi_lcpll_get_freq`: the link clock for each code, doubled. */
static unsigned link_clock_of(unsigned code)
{
    switch (code) {
    case 0:  return 270000;     /* DPLL_CTRL1_LINK_RATE_2700 */
    case 1:  return 135000;     /* _1350 */
    case 2:  return 81000;      /* _810 */
    case 3:  return 162000;     /* _1620 */
    case 4:  return 108000;     /* _1080 */
    case 5:  return 216000;     /* _2160 */
    default: return 0;
    }
}

void igpu_decode_dpll_ctrl1(uint32_t v, unsigned dpll, struct igpu_dpll *out)
{
    uint32_t bits = (dpll < IGPU_DPLLS) ? (v >> (dpll * 6u)) & 0x3Fu : 0;

    out->override = (bits & CTRL1_OVERRIDE) != 0;
    out->ssc = (bits & CTRL1_SSC) != 0;
    out->hdmi = (bits & CTRL1_HDMI) != 0;
    out->rate_code = (bits >> CTRL1_RATE_SHIFT) & CTRL1_RATE_MASK;
    out->port_clock = out->hdmi ? 0 : link_clock_of(out->rate_code) * 2u;
}

bool igpu_decode_dpll_ctrl2(uint32_t v, unsigned port, unsigned *dpll, bool *clock_off)
{
    if (port >= IGPU_DDIS) {
        return false;
    }

    *clock_off = (v & (1u << (port + 15u))) != 0;

    if ((v & (1u << (port * 3u))) == 0) {
        return false;
    }

    *dpll = (v >> (port * 3u + 1u)) & 0x3u;
    return true;
}

unsigned igpu_wrpll_khz(uint32_t cfgcr1, uint32_t cfgcr2, unsigned ref_khz)
{
    unsigned p0, p1, p2;
    uint64_t dco;

    switch ((cfgcr2 >> CFGCR2_PDIV_SHIFT) & CFGCR2_PDIV_MASK) {
    case 0:  p0 = 1; break;
    case 1:  p0 = 2; break;
    case 2:  p0 = 3; break;
    case 4:
    case 5:  p0 = 7; break;     /* 5: an ASUS firmware's, which the hardware takes as 7 */
    default: return 0;
    }

    switch ((cfgcr2 >> CFGCR2_KDIV_SHIFT) & CFGCR2_KDIV_MASK) {
    case 0:  p2 = 5; break;
    case 1:  p2 = 2; break;
    case 2:  p2 = 3; break;
    default: p2 = 1; break;
    }

    p1 = (cfgcr2 & CFGCR2_QDIV_MODE) ? (cfgcr2 >> CFGCR2_QDIV_SHIFT) & CFGCR2_QDIV_MASK : 1;

    if (p1 == 0) {
        return 0;
    }

    dco = (uint64_t)(cfgcr1 & CFGCR1_INTEGER_MASK) * ref_khz;
    dco += (uint64_t)((cfgcr1 >> CFGCR1_FRACTION_SHIFT) & CFGCR1_FRACTION_MASK) * ref_khz / 0x8000u;

    return (unsigned)(dco / ((uint64_t)p0 * p1 * p2 * 5u));
}

void igpu_decode_ddi_buf(uint32_t v, struct igpu_ddi_buf *out)
{
    out->enabled = (v & BUF_ENABLE) != 0;
    out->idle = (v & BUF_IDLE) != 0;
    out->reversed = (v & BUF_REVERSAL) != 0;
    out->a_four_lanes = (v & BUF_A_4_LANES) != 0;
    out->detected = (v & BUF_DETECTED) != 0;
    out->trans_select = (v >> BUF_TRANS_SHIFT) & BUF_TRANS_MASK;
    out->lanes = ((v >> BUF_WIDTH_SHIFT) & BUF_WIDTH_MASK) + 1;
}

void igpu_decode_cdclk(uint32_t cdclk_ctl, uint32_t lcpll1_ctl, uint32_t dpll_ctrl1,
                       struct igpu_cdclk *out)
{
    static const unsigned at_8100[4] = { 450000, 540000, 337500, 675000 };
    static const unsigned at_8640[4] = { 432000, 540000, 308571, 617143 };
    unsigned sel = (cdclk_ctl >> CDCLK_SEL_SHIFT) & CDCLK_SEL_MASK;
    struct igpu_dpll pll;

    out->pll_on = (lcpll1_ctl & (LCPLL_ENABLE | LCPLL_LOCK)) == (LCPLL_ENABLE | LCPLL_LOCK);
    out->vco_khz = 0;
    out->cdclk_khz = IGPU_REF_KHZ;      /* `skl_get_cdclk`: the bypass, with no VCO */
    out->decimal_khz = (cdclk_ctl & CDCLK_DECIMAL_MASK) * 500u + 1000u;

    igpu_decode_dpll_ctrl1(dpll_ctrl1, 0, &pll);

    /* `skl_dpll0_update` takes the VCO only from DPLL 0 in DP mode with its
     * override on and SSC off. */
    if (!out->pll_on || !pll.override || pll.ssc || pll.hdmi) {
        return;
    }

    switch (pll.rate_code) {
    case 0: case 1: case 2: case 3:
        out->vco_khz = VCO_8100;
        out->cdclk_khz = at_8100[sel];
        break;
    case 4: case 5:
        out->vco_khz = VCO_8640;
        out->cdclk_khz = at_8640[sel];
        break;
    default:
        break;
    }
}

/* `skl_calc_cdclk`: the clock for a VCO at least as fast as asked for. */
static unsigned skl_calc_cdclk(unsigned min_khz, unsigned vco_khz)
{
    if (vco_khz == VCO_8640) {
        return min_khz > 540000 ? 617143 : min_khz > 432000 ? 540000
             : min_khz > 308571 ? 432000 : 308571;
    }

    return min_khz > 540000 ? 675000 : min_khz > 450000 ? 540000
         : min_khz > 337500 ? 450000 : 337500;
}

unsigned igpu_max_cdclk_khz(uint32_t skl_dfsm, unsigned vco_khz)
{
    static const unsigned limit[4] = { 617143, 540000, 432000, 308571 };

    return skl_calc_cdclk(limit[(skl_dfsm >> DFSM_LIMIT_SHIFT) & DFSM_LIMIT_MASK], vco_khz);
}

unsigned igpu_dp_pixel_khz(const struct igpu_m_n *mn, unsigned port_clock)
{
    uint64_t num, den;

    if (mn->link_n == 0) {
        return 0;
    }

    /* `intel_dotclock_calculate`: m * link_freq * 10 over n * symbol size,
     * rounded up; the symbol is ten bits below UHBR, which this engine
     * does not have. */
    num = (uint64_t)mn->link_m * port_clock * 10u;
    den = (uint64_t)mn->link_n * 10u;
    return (unsigned)((num + den - 1) / den);
}

unsigned igpu_hdmi_pixel_khz(unsigned port_khz, unsigned bpc)
{
    unsigned bpp = bpc * 3u;

    if (bpp <= 24) {
        return port_khz;
    }

    return (unsigned)(((uint64_t)port_khz * 24u + bpp / 2u) / bpp);
}

unsigned igpu_refresh_mhz(unsigned pixel_khz, unsigned htotal, unsigned vtotal)
{
    uint64_t frame = (uint64_t)htotal * vtotal;

    if (frame == 0) {
        return 0;
    }

    return (unsigned)(((uint64_t)pixel_khz * 1000000u + frame / 2u) / frame);
}

unsigned igpu_dp_bpp_milli(const struct igpu_m_n *mn, unsigned port_clock,
                           unsigned lanes, unsigned pixel_khz)
{
    uint64_t num, den;

    if (mn->data_n == 0 || pixel_khz == 0) {
        return 0;
    }

    num = (uint64_t)mn->data_m * port_clock * lanes * 8u * 1000u;
    den = (uint64_t)mn->data_n * pixel_khz;
    return (unsigned)((num + den / 2u) / den);
}

void igpu_decode_output(const struct igpu_output_regs *r, struct igpu_output *out)
{
    igpu_decode_ddi_func(r->ddi_func_ctl, r->edp_transcoder, &out->func);
    igpu_decode_timings(&r->timing, &out->t);
    igpu_decode_m_n(r->data_m1, r->data_n1, r->link_m1, r->link_n1, &out->mn);
    igpu_decode_ddi_buf(r->ddi_buf_ctl, &out->buf);

    out->is_dp = out->func.mode == IGPU_DDI_DP_SST || out->func.mode == IGPU_DDI_DP_MST;
    out->dpll = 0;
    out->dpll_clock_off = false;
    out->dpll_known = igpu_decode_dpll_ctrl2(r->dpll_ctrl2, out->func.port,
                                             &out->dpll, &out->dpll_clock_off);
    out->port_clock = 0;
    out->pixel_khz = 0;
    out->refresh_mhz = 0;
    out->bpp_milli = 0;

    if (!out->dpll_known) {
        igpu_decode_dpll_ctrl1(0, 0, &out->pll);
        return;
    }

    igpu_decode_dpll_ctrl1(r->dpll_ctrl1, out->dpll, &out->pll);

    if (out->pll.hdmi) {
        out->port_clock = igpu_wrpll_khz(r->cfgcr1, r->cfgcr2, IGPU_REF_KHZ);
    } else {
        out->port_clock = out->pll.port_clock;
    }

    if (out->is_dp) {
        out->pixel_khz = igpu_dp_pixel_khz(&out->mn, out->port_clock);
        out->bpp_milli = igpu_dp_bpp_milli(&out->mn, out->port_clock, out->func.lanes,
                                           out->pixel_khz);
    } else if (out->func.mode == IGPU_DDI_HDMI || out->func.mode == IGPU_DDI_DVI) {
        out->pixel_khz = igpu_hdmi_pixel_khz(out->port_clock,
                                             out->func.mode == IGPU_DDI_HDMI ? out->func.bpc : 8u);
    }

    out->refresh_mhz = igpu_refresh_mhz(out->pixel_khz, out->t.htotal, out->t.vtotal);
}
