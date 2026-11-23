/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_DISPLAY_INTELGPU_DISPLAY_H
#define KOSMOS_DRIVERS_DISPLAY_INTELGPU_DISPLAY_H

/*
 * **What the firmware lit, read back** (`docs/displaymodes.md` D1): on a
 * Gen9 display engine - Skylake and Kaby Lake, the M700's HD 530 - which
 * transcoder drives which DDI, whether as DisplayPort or HDMI, at what
 * link rate and how many lanes, the display clock, the transcoder's six
 * timing registers, and from them and the M/N values the pixel clock and
 * the refresh rate.
 *
 * Its own file beside `intelgpu_decode.c`, and for the same reason: no
 * hardware and no system call, so it is held to numbers on the Mac
 * (`tools/test_intelgpu_display.c`) and to the M700 by what it says there.
 * Nothing here writes; D6 is where a mode is set.
 *
 * **Every offset and field is Linux's i915, v6.12** - `build/downloads/
 * i915-6.12/i915_reg.h` and, fetched for this, `intel_display_device.c`,
 * `intel_display.c`, `intel_ddi.c`, `intel_dpll_mgr.c` and
 * `intel_cdclk.c` from the same tag. The file and the macro or function
 * are named beside each one below. Where i915 has two readings of a field
 * for two generations, the one taken is the one i915 takes for display
 * version 9.
 */

#include <stdbool.h>
#include <stdint.h>

/*
 * **Where.** A transcoder's registers are transcoder A's plus the
 * transcoder's offset less A's (`_MMIO_TRANS2`, with Skylake's offsets
 * from `HSW_PIPE_OFFSETS` in `intel_display_device.c`: A 0x60000, B
 * 0x61000, C 0x62000, eDP 0x6f000). Transcoders A, B and C belong to the
 * pipe of the same letter; the eDP one is joined to whichever pipe its
 * `TRANS_DDI_FUNC_CTL` names (`hsw_enabled_transcoders`).
 */
#define IGPU_TRANSCODER_A       0x60000u
#define IGPU_TRANSCODER_B       0x61000u
#define IGPU_TRANSCODER_C       0x62000u
#define IGPU_TRANSCODER_EDP     0x6f000u
#define IGPU_TRANSCODERS        4u          /* A, B, C, eDP */

/* From a transcoder's base (`i915_reg.h`'s `_TRANS_*_A` and `_PIPEA_*` less
 * 0x60000). */
#define IGPU_TRANS_HTOTAL       0x000u      /* _TRANS_HTOTAL_A 0x60000 */
#define IGPU_TRANS_HBLANK       0x004u      /* _TRANS_HBLANK_A 0x60004 */
#define IGPU_TRANS_HSYNC        0x008u      /* _TRANS_HSYNC_A 0x60008 */
#define IGPU_TRANS_VTOTAL       0x00cu      /* _TRANS_VTOTAL_A 0x6000c */
#define IGPU_TRANS_VBLANK       0x010u      /* _TRANS_VBLANK_A 0x60010 */
#define IGPU_TRANS_VSYNC        0x014u      /* _TRANS_VSYNC_A 0x60014 */
#define IGPU_TRANS_DATA_M1      0x030u      /* _PIPEA_DATA_M1 0x60030 */
#define IGPU_TRANS_DATA_N1      0x034u      /* _PIPEA_DATA_N1 0x60034 */
#define IGPU_TRANS_LINK_M1      0x040u      /* _PIPEA_LINK_M1 0x60040 */
#define IGPU_TRANS_LINK_N1      0x044u      /* _PIPEA_LINK_N1 0x60044 */
#define IGPU_TRANS_DDI_FUNC_CTL 0x400u      /* _TRANS_DDI_FUNC_CTL_A 0x60400 */

/* A DDI's buffer: `_DDI_BUF_CTL_A` 0x64000, `_DDI_BUF_CTL_B` 0x64100, so
 * 0x100 a port (`_MMIO_PORT`). Skylake has DDI A to E. */
#define IGPU_DDI_BUF_CTL_A      0x64000u
#define IGPU_DDI_STRIDE         0x100u
#define IGPU_DDIS               5u

/* The shared DPLLs (`i915_reg.h`, "SKL Clocks"): one control register for
 * all four, one for which DPLL each DDI takes, one for their locks, and
 * an HDMI-mode DPLL's dividers - DPLL 1 to 3 only, 8 bytes apart
 * (`_DPLL1_CFGCR1` 0x6C040, `_DPLL2_CFGCR1` 0x6C048). */
#define IGPU_DPLL_CTRL1         0x6c058u    /* DPLL_CTRL1 */
#define IGPU_DPLL_CTRL2         0x6c05cu    /* DPLL_CTRL2 */
#define IGPU_DPLL_STATUS        0x6c060u    /* DPLL_STATUS: DPLL_LOCK(id) is bit id * 8 */
#define IGPU_DPLL1_CFGCR1       0x6c040u    /* _DPLL1_CFGCR1 */
#define IGPU_DPLL1_CFGCR2       0x6c044u    /* _DPLL1_CFGCR2 */
#define IGPU_DPLL_CFGCR_STRIDE  0x8u
#define IGPU_DPLLS              4u          /* DPLL 0 drives the display clock and eDP */

/* The display clock: `CDCLK_CTL`, DPLL 0's enable (`LCPLL1_CTL`), and the
 * fuse that limits it (`SKL_DFSM`). */
#define IGPU_CDCLK_CTL          0x46000u
#define IGPU_LCPLL1_CTL         0x46010u
#define IGPU_SKL_DFSM           0x51000u

/* The reference the WRPLLs divide from: 24 MHz on Skylake
 * (`skl_update_dpll_ref_clks` takes `cdclk.hw.ref`, which
 * `skl_dpll0_update` sets to 24000). */
#define IGPU_REF_KHZ            24000u

/* The offsets of one DDI's, one DPLL's and one transcoder's registers. */
uint32_t igpu_ddi_buf_ctl_at(unsigned port);
uint32_t igpu_dpll_cfgcr1_at(unsigned dpll);    /* 0 for DPLL 0, which has none */
uint32_t igpu_dpll_cfgcr2_at(unsigned dpll);
uint32_t igpu_transcoder_at(unsigned transcoder);   /* 0 A, 1 B, 2 C, 3 eDP */
const char *igpu_transcoder_name(unsigned transcoder);
const char *igpu_port_name(unsigned port);          /* "DDI A" ... */

/*
 * **`TRANS_DDI_FUNC_CTL`** (`i915_reg.h`): enable 31
 * (`TRANS_DDI_FUNC_ENABLE`), the DDI in 30:28 (`TRANS_DDI_PORT_MASK`,
 * before Tiger Lake), the mode in 26:24 (`TRANS_DDI_MODE_SELECT_*`:
 * 0 HDMI, 1 DVI, 2 DP SST, 3 DP MST, 4 FDI), bits a colour in 22:20
 * (`TRANS_DDI_BPC_*`: 0 is 8, 1 is 10, 2 is 6, 3 is 12), the sync
 * polarities in 17 and 16 (`TRANS_DDI_PVSYNC`, `TRANS_DDI_PHSYNC`), the eDP
 * transcoder's pipe in 14:12 (`TRANS_DDI_EDP_INPUT_*`) and the lanes less
 * one in 3:1 (`TRANS_DDI_PORT_WIDTH_MASK`; `intel_ddi_read_func_ctl`
 * reads it for DP and takes four for HDMI and DVI below version 14).
 */
enum igpu_ddi_mode {
    IGPU_DDI_HDMI = 0,
    IGPU_DDI_DVI = 1,
    IGPU_DDI_DP_SST = 2,
    IGPU_DDI_DP_MST = 3,
    IGPU_DDI_FDI = 4,
};

struct igpu_ddi_func {
    bool        enabled;
    unsigned    port;           /* 0 is DDI A */
    unsigned    mode;           /* enum igpu_ddi_mode, or 5-7 unnamed */
    const char *mode_name;      /* "HDMI", "DVI", "DisplayPort", "DP MST", "FDI", or NULL */
    unsigned    bpc;            /* 6, 8, 10 or 12; 0 for a code i915 does not name */
    unsigned    lanes;
    bool        hsync_positive, vsync_positive;
    int         edp_pipe;       /* the eDP transcoder's pipe, 0 A; -1 for the others */
};

void igpu_decode_ddi_func(uint32_t v, bool edp_transcoder, struct igpu_ddi_func *out);

/*
 * **The six timing registers** (`intel_display_regs.h`, `TRANS_HTOTAL` to
 * `TRANS_VSYNC`): each two halves of 16 bits, the end or total in 31:16
 * and the start or active in 15:0, every one less one -
 * `intel_get_transcoder_timings` adds the one back to all twelve.
 */
struct igpu_timing_regs {
    uint32_t htotal, hblank, hsync, vtotal, vblank, vsync;
};

struct igpu_timings {
    unsigned hactive, htotal, hblank_start, hblank_end, hsync_start, hsync_end;
    unsigned vactive, vtotal, vblank_start, vblank_end, vsync_start, vsync_end;
};

void igpu_decode_timings(const struct igpu_timing_regs *r, struct igpu_timings *out);

/*
 * **M/N** (`intel_get_m_n`): each value in 23:0 (`DATA_LINK_M_N_MASK`),
 * and the transfer unit's size less one in `DATA_M1`'s 30:25
 * (`TU_SIZE_MASK`). Link M over link N is the pixel clock over the link's
 * symbol clock; data M over data N the pixels' bits over what the lanes
 * carry.
 */
struct igpu_m_n {
    unsigned tu;
    uint32_t data_m, data_n, link_m, link_n;
};

void igpu_decode_m_n(uint32_t data_m1, uint32_t data_n1, uint32_t link_m1,
                     uint32_t link_n1, struct igpu_m_n *out);

/*
 * **`DPLL_CTRL1`** holds six bits for each DPLL, at `id * 6`
 * (`skl_ddi_pll_get_hw_state`): override 0, the link rate in 3:1, SSC 4,
 * HDMI mode 5. The link rate is a code (`DPLL_CTRL1_LINK_RATE_*`: 0 is
 * 2700, 1 1350, 2 810, 3 1620, 4 1080, 5 2160), and the port clock is
 * twice its link clock (`skl_ddi_lcpll_get_freq`) - i915's port clock is
 * the link's rate in 10 kbit/s, so 2700 is HBR2, 5.4 Gbit/s a lane.
 */
struct igpu_dpll {
    bool     override, ssc, hdmi;
    unsigned rate_code;
    unsigned port_clock;        /* 10 kbit/s; 0 for an HDMI-mode DPLL, or a code not named */
};

void igpu_decode_dpll_ctrl1(uint32_t v, unsigned dpll, struct igpu_dpll *out);

/* `DPLL_CTRL2`: which DPLL a DDI takes, in `port * 3 + 1` (two bits), when
 * its override, `port * 3`, is set (`skl_ddi_get_pll`); and whether its
 * clock is off, bit `port + 15`. False when the override is clear: i915
 * then knows no DPLL for it either. */
bool igpu_decode_dpll_ctrl2(uint32_t v, unsigned port, unsigned *dpll, bool *clock_off);

/*
 * **An HDMI-mode DPLL's frequency** (`skl_ddi_wrpll_get_freq`): the DCO is
 * `DPLL_CFGCR1`'s integer (8:0) and fraction (23:9, over 0x8000) times the
 * reference, divided by `DPLL_CFGCR2`'s P (4:2: 0 is 1, 1 is 2, 2 is 3, 4
 * or 5 is 7), Q (15:8, when 7 says it is used) and K (6:5: 0 is 5, 1 is
 * 2, 2 is 3, 3 is 1), and by five. That is the TMDS clock in kHz. 0 for a
 * divider i915 does not name.
 */
unsigned igpu_wrpll_khz(uint32_t cfgcr1, uint32_t cfgcr2, unsigned ref_khz);

/*
 * **`DDI_BUF_CTL`** (`i915_reg.h`): enable 31, the voltage swing's entry in
 * 27:24 (`DDI_BUF_TRANS_SELECT`), lanes reversed 16, idle 7, DDI A given
 * all four lanes 4 (`DDI_A_4_LANES`), lanes less one in 3:1
 * (`DDI_PORT_WIDTH`, which `intel_ddi_init_dp_buf_reg` writes for DP), and
 * a display found at boot 0 (`DDI_INIT_DISPLAY_DETECTED`, which i915 reads
 * on DDI A only).
 */
struct igpu_ddi_buf {
    bool     enabled, idle, reversed, a_four_lanes, detected;
    unsigned trans_select;
    unsigned lanes;
};

void igpu_decode_ddi_buf(uint32_t v, struct igpu_ddi_buf *out);

/*
 * **The display clock** (`skl_dpll0_update`, `skl_get_cdclk`): DPLL 0 must
 * be on and locked (`LCPLL1_CTL`'s 31 and 30) and in DP mode with its
 * override set, and its link rate says the VCO - 8.1 GHz for 810, 1350,
 * 1620 and 2700, 8.64 GHz for 1080 and 2160. `CDCLK_CTL`'s 27:26 then
 * picks one of four clocks for that VCO: 450 or 432, 540, 337.5 or 308.571,
 * 675 or 617.143 MHz. With DPLL 0 off, the clock is the 24 MHz reference.
 * `CDCLK_CTL`'s 10:0 is the same clock written for the hardware's own
 * use, (kHz - 1000) / 500 (`skl_cdclk_decimal`) - read back beside the
 * other as a check.
 */
struct igpu_cdclk {
    bool     pll_on;
    unsigned vco_khz;           /* 0 when DPLL 0 is off or says no VCO i915 names */
    unsigned cdclk_khz;
    unsigned decimal_khz;       /* from 10:0 */
};

void igpu_decode_cdclk(uint32_t cdclk_ctl, uint32_t lcpll1_ctl, uint32_t dpll_ctrl1,
                       struct igpu_cdclk *out);

/*
 * **The fastest display clock this part may run** (`intel_update_max_cdclk`
 * for display version 9): the fuse in `SKL_DFSM`'s 24:23 (0 675, 1 540,
 * 2 450, 3 337.5), its lower figure for an 8.64 GHz VCO, then the clock
 * `skl_calc_cdclk` gives that for the VCO in use. On Skylake the fastest
 * pixel clock is this same number (`intel_compute_max_dotclk`).
 */
unsigned igpu_max_cdclk_khz(uint32_t skl_dfsm, unsigned vco_khz);

/*
 * **The pixel clock and the refresh.** For DisplayPort,
 * `intel_dotclock_calculate`: link M times the port clock over link N,
 * rounded up - the symbol being ten bits at every rate this engine has.
 * For HDMI, `intel_crtc_dotclock`: the port clock, times 24 over the
 * pixel's bits above 8 a colour. The refresh, in millihertz, is the pixel
 * clock over the total pixels a frame, rounded to the nearest.
 */
unsigned igpu_dp_pixel_khz(const struct igpu_m_n *mn, unsigned port_clock);
unsigned igpu_hdmi_pixel_khz(unsigned port_khz, unsigned bpc);
unsigned igpu_refresh_mhz(unsigned pixel_khz, unsigned htotal, unsigned vtotal);

/* What data M/N says a pixel is, in thousandths of a bit: its bits over
 * what four lanes of eight-bit symbols carry at the symbol clock, turned
 * round. 24000 for 8 bits a colour; a check on `TRANS_DDI_FUNC_CTL`'s
 * bits a colour, slightly over when the firmware allowed for overhead. */
unsigned igpu_dp_bpp_milli(const struct igpu_m_n *mn, unsigned port_clock,
                           unsigned lanes, unsigned pixel_khz);

/*
 * **The whole of an output**, from the registers one transcoder and its
 * DDI name. The driver reads in two passes - the transcoder's, then the
 * DDI's buffer, `DPLL_CTRL1` and `DPLL_CTRL2`, and the DPLL's dividers
 * from the port and the DPLL the first pass named - and hands all of it
 * here.
 */
struct igpu_output_regs {
    bool                    edp_transcoder;
    uint32_t                ddi_func_ctl;
    struct igpu_timing_regs timing;
    uint32_t                data_m1, data_n1, link_m1, link_n1;
    uint32_t                ddi_buf_ctl;        /* the DDI `ddi_func_ctl` names */
    uint32_t                dpll_ctrl1, dpll_ctrl2;
    uint32_t                cfgcr1, cfgcr2;     /* the DPLL `dpll_ctrl2` names; 0 for DPLL 0 */
};

struct igpu_output {
    struct igpu_ddi_func func;
    struct igpu_timings  t;
    struct igpu_m_n      mn;
    struct igpu_ddi_buf  buf;
    bool                 dpll_known;
    unsigned             dpll;
    bool                 dpll_clock_off;
    struct igpu_dpll     pll;
    unsigned             port_clock;    /* DP: 10 kbit/s a lane; HDMI: the TMDS clock in kHz */
    bool                 is_dp;
    unsigned             pixel_khz;     /* 0 when it cannot be known */
    unsigned             refresh_mhz;
    unsigned             bpp_milli;     /* DP only */
};

void igpu_decode_output(const struct igpu_output_regs *r, struct igpu_output *out);

#endif
