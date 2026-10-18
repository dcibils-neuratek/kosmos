/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_DISPLAY_INTELGPU_DECODE_H
#define KOSMOS_DRIVERS_DISPLAY_INTELGPU_DECODE_H

/*
 * What an Intel Gen9 display engine's registers say - a pipe, a plane, a
 * cursor, an entry of the global GTT - for the M700's display driver
 * (`docs/m700-2d.md` step 2: find it and look, writing nothing).
 *
 * Its own file, with no hardware and no system calls in it, as
 * `backlight_decode.c` is: QEMU has no Intel GPU, so this is held to
 * numbers on the Mac (`tools/test_intelgpu_decode.c`) and to the M700 by
 * what it says there.
 *
 * **The layout is Linux's i915, checked line by line** - `build/downloads/
 * i915/`, from the kernel's tree: `skl_universal_plane_regs.h` (`PLANE_CTL`
 * enable 31, format 27:24 before Ice Lake, tiling 12:10; `PLANE_STRIDE`
 * 11:0; `PLANE_SIZE`, `PLANE_POS` and `PLANE_OFFSET` a height and a width;
 * `PLANE_SURF` the GTT address, 31:12), `skl_universal_plane.c`
 * (`skl_plane_stride_mult`: 64 bytes linear, else a tile's width -
 * `intel_fb.c`: 512 X, 128 Y), `intel_cursor_regs.h` (`CURCNTR`'s mode in
 * 0x27, `CURPOS`'s signs in 31 and 15), `intel_display_regs.h` (`TRANSCONF`
 * enable 31, `PIPESRC` width and height less one), and `intel_ggtt.c` and
 * `intel_gtt.h` (the GTT's entries from GTTMMADR's half way, eight bytes,
 * present in bit 0, the page in 45:12).
 */

#include <stdbool.h>
#include <stdint.h>

/* Where, inside GTTMMADR: the registers of pipe A, the next pipe 0x1000 on. */
#define IGPU_PIPE_STRIDE        0x1000u
#define IGPU_PIPES              3u
#define IGPU_TRANSCONF_A        0x70008u
#define IGPU_PIPESRC_A          0x6001cu
#define IGPU_PLANE_CTL_1_A      0x70180u
#define IGPU_PLANE_STRIDE_1_A   0x70188u
#define IGPU_PLANE_POS_1_A      0x7018cu
#define IGPU_PLANE_SIZE_1_A     0x70190u
#define IGPU_PLANE_SURF_1_A     0x7019cu
#define IGPU_PLANE_OFFSET_1_A   0x701a4u
#define IGPU_PLANE_STRIDE       0x100u      /* plane 2 is 0x70280 */
#define IGPU_PLANES             3u          /* the M700's pipes have three */
#define IGPU_PLANE_SURFLIVE_1_A 0x701acu    /* the surface being scanned now */
#define IGPU_PIPE_FRMCOUNT_A    0x70040u    /* frames the pipe has scanned */
/* The GT's clock and sleep (`intel_gt_regs.h`, `intel_mchbar_regs.h`, and
 * `intel_rps.c` and `intel_rc6.c` for their units on Gen9). */
#define IGPU_RPSTAT1            0x0a01cu    /* GEN6_RPSTAT1: CAGF in 31:23, 50/3 MHz */
#define IGPU_RP_STATE_CAP       0x145998u   /* MCHBAR mirror + 0x5998: RP0 7:0, RPn 23:16, 50 MHz */
#define IGPU_RC6_RESIDENCY      0x138108u   /* GEN6_GT_GFX_RC6: 1280 ns a count */
/* Waking the GT (`intel_gt_regs.h`; `intel_uncore.c`'s `__gen9_fw_ranges`
 * puts the blitter's registers, 0x22000-0x243ff, in the GT's domain). The
 * request is a masked write - bit 16 + n says bit n is meant. */
#define IGPU_FORCEWAKE_GT       0x0a188u    /* FORCEWAKE_GT_GEN9 */
#define IGPU_FORCEWAKE_ACK_GT   0x130044u   /* FORCEWAKE_ACK_GT_GEN9 */
#define IGPU_FORCEWAKE_KERNEL   (1u << 0)
/* An engine's ring (`intel_engine_regs.h`), from its base: the blitter's
 * is BLT_RING_BASE (`i915_reg.h`). */
#define IGPU_BLT_BASE           0x22000u
#define IGPU_RING_TAIL          0x30u
#define IGPU_RING_HEAD          0x34u
#define IGPU_RING_START         0x38u
#define IGPU_RING_CTL           0x3cu       /* (pages - 1) << 12 | RING_VALID */
#define IGPU_RING_ACTHD         0x74u
#define IGPU_RING_HWS_PGA       0x80u
#define IGPU_RING_HWSTAM        0x98u
#define IGPU_RING_MI_MODE       0x9cu       /* masked: STOP_RING 8, MODE_IDLE 9 */
#define IGPU_RING_ESR           0xb8u
#define IGPU_RING_MODE          0x29cu      /* RING_MODE_GEN7: GFX_RUN_LIST_ENABLE 15 */
#define IGPU_RING_TIMESTAMP     0x358u
#define IGPU_RING_VALID         (1u << 0)
#define IGPU_STOP_RING          (1u << 8)
/* Commands (`intel_gpu_commands.h`): MI_INSTR(opcode, flags) is opcode << 23
 * | flags; MI_STORE_DWORD_IMM_GEN4 is opcode 0x20 with length 2 - four
 * dwords: itself, the address low and high, the value - and MI_USE_GGTT
 * says the address is in the global GTT. */
#define IGPU_MI_NOOP            0u
#define IGPU_MI_STORE_DWORD     ((0x20u << 23) | 2u)
#define IGPU_MI_USE_GGTT        (1u << 22)
#define IGPU_PIPE_FLIPCOUNT_A   0x70044u    /* flips the pipe has latched */
#define IGPU_CURCNTR_A          0x70080u
#define IGPU_CURBASE_A          0x70084u
#define IGPU_CURPOS_A           0x70088u
/* The display buffer's shares and their watermarks (`skl_universal_plane_
 * regs.h`, `intel_cursor_regs.h`): plane 1's, the next plane 0x100 on, and
 * the cursor's; eight levels each, a word apart, and one for transitions. */
#define IGPU_PLANE_BUF_CFG_1_A  0x7027cu
#define IGPU_PLANE_WM_1_A_0     0x70240u
#define IGPU_PLANE_WM_TRANS_1_A 0x70268u
#define IGPU_CUR_BUF_CFG_A      0x7017cu
#define IGPU_CUR_WM_A_0         0x70140u
#define IGPU_CUR_WM_TRANS_A     0x70168u
#define IGPU_WM_LEVELS          8u
#define IGPU_GTT_AT             0x800000u   /* GTTMMADR's second half, Gen8 on */
#define IGPU_REGISTERS_BYTES    0x200000u   /* the first 2 MB */

/* A plane's registers, as read. */
struct igpu_plane_regs {
    uint32_t ctl, stride, pos, size, surf, offset;
};

/* What they say. */
struct igpu_plane {
    bool     enabled;
    const char *format;     /* "XRGB8888", "ARGB8888", ... or "format N" */
    unsigned format_code;   /* bits 27:24 */
    const char *tiling;     /* "linear", "X", "Y", "Yf" or "tiling N" */
    unsigned width, height; /* from PLANE_SIZE */
    unsigned x, y;          /* on the pipe, from PLANE_POS */
    unsigned stride_bytes;  /* PLANE_STRIDE times its unit */
    uint32_t surface;       /* the GTT address it scans out from */
};

void igpu_decode_plane(const struct igpu_plane_regs *r, struct igpu_plane *out);

/* A pipe's source size: `PIPESRC` holds each less one. */
void igpu_decode_pipesrc(uint32_t v, unsigned *width, unsigned *height);

/* A cursor's control: its mode's name, or NULL when disabled. */
const char *igpu_cursor_mode(uint32_t curcntr);

/* `CURPOS`: x and y, each signed by its own bit. */
void igpu_decode_curpos(uint32_t v, int *x, int *y);

/* A share of the display buffer: blocks from `start` to `end`, inclusive
 * (`PLANE_BUF_START` 12:0, `PLANE_BUF_END` 28:16); none when both are 0. */
void igpu_decode_buf(uint32_t v, unsigned *start, unsigned *end);

/* A watermark: enabled (31), lines (26:14), blocks (12:0). */
void igpu_decode_wm(uint32_t v, bool *enabled, unsigned *lines, unsigned *blocks);

/* One global GTT entry: whether present, and the physical page it maps. */
bool igpu_decode_gtt(uint64_t entry, uint64_t *physical);

#endif
