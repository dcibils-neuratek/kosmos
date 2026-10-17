/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The display engine's registers, read into what they mean
 * (`intelgpu_decode.h` has where every bit comes from).
 */

#include <stddef.h>

#include "intelgpu_decode.h"

#define PLANE_ENABLE        (1u << 31)
#define FORMAT_SHIFT        24
#define FORMAT_MASK         0xFu
#define TILING_SHIFT        10
#define TILING_MASK         0x7u
#define STRIDE_MASK         0xFFFu
#define SURF_MASK           0xFFFFF000u

#define CURSOR_MODE_MASK    0x27u
#define CURSOR_POS_Y_SIGN   (1u << 31)
#define CURSOR_POS_X_SIGN   (1u << 15)
#define CURSOR_POS_MASK     0x7FFFu

#define GTT_PRESENT         1ull
#define GTT_ADDRESS_MASK    0x00003FFFFFFFF000ull   /* 45:12 */

/* `PLANE_CTL_FORMAT_*` before Ice Lake: `skl_universal_plane_regs.h`. The
 * alpha forms are XRGB with `PLANE_CTL_ALPHA_*` beside, so one name here. */
static const char *format_name(unsigned code)
{
    switch (code) {
    case 0:  return "YUV 4:2:2";
    case 1:  return "NV12";
    case 2:  return "XRGB2101010";
    case 3:  return "P010";
    case 4:  return "XRGB8888";
    case 5:  return "P012";
    case 6:  return "XRGB16161616F";
    case 7:  return "P016";
    case 8:  return "XYUV";
    case 12: return "indexed 8";
    case 14: return "RGB565";
    default: return NULL;
    }
}

static const char *tiling_name(unsigned code)
{
    switch (code) {
    case 0:  return "linear";
    case 1:  return "X";
    case 4:  return "Y";
    case 5:  return "Yf";
    default: return NULL;
    }
}

/* `skl_plane_stride_mult`: 64 bytes linear, else a tile's width in bytes. */
static unsigned stride_unit(unsigned tiling)
{
    switch (tiling) {
    case 0:  return 64;
    case 1:  return 512;
    case 4:
    case 5:  return 128;
    default: return 0;
    }
}

void igpu_decode_plane(const struct igpu_plane_regs *r, struct igpu_plane *out)
{
    unsigned format = (r->ctl >> FORMAT_SHIFT) & FORMAT_MASK;
    unsigned tiling = (r->ctl >> TILING_SHIFT) & TILING_MASK;

    out->enabled = (r->ctl & PLANE_ENABLE) != 0;
    out->format_code = format;
    out->format = format_name(format);
    out->tiling = tiling_name(tiling);
    out->width = (r->size & 0xFFFFu) + 1;
    out->height = (r->size >> 16) + 1;
    out->x = r->pos & 0xFFFFu;
    out->y = r->pos >> 16;
    out->stride_bytes = (r->stride & STRIDE_MASK) * stride_unit(tiling);
    out->surface = r->surf & SURF_MASK;
}

void igpu_decode_pipesrc(uint32_t v, unsigned *width, unsigned *height)
{
    *width = (v >> 16) + 1;
    *height = (v & 0xFFFFu) + 1;
}

const char *igpu_cursor_mode(uint32_t curcntr)
{
    switch (curcntr & CURSOR_MODE_MASK) {
    case 0x00: return NULL;
    case 0x02: return "128x128 32-bit AND/XOR";
    case 0x03: return "256x256 32-bit AND/XOR";
    case 0x04: return "64x64 2-bit";
    case 0x07: return "64x64 32-bit AND/XOR";
    case 0x22: return "128x128 ARGB";
    case 0x23: return "256x256 ARGB";
    case 0x27: return "64x64 ARGB";
    default:   return "another mode";
    }
}

void igpu_decode_curpos(uint32_t v, int *x, int *y)
{
    *x = (int)(v & CURSOR_POS_MASK);
    *y = (int)((v >> 16) & CURSOR_POS_MASK);

    if (v & CURSOR_POS_X_SIGN) {
        *x = -*x;
    }

    if (v & CURSOR_POS_Y_SIGN) {
        *y = -*y;
    }
}

bool igpu_decode_gtt(uint64_t entry, uint64_t *physical)
{
    *physical = entry & GTT_ADDRESS_MASK;
    return (entry & GTT_PRESENT) != 0;
}

void igpu_decode_buf(uint32_t v, unsigned *start, unsigned *end)
{
    *start = v & 0x1FFFu;
    *end = (v >> 16) & 0x1FFFu;
}

void igpu_decode_wm(uint32_t v, bool *enabled, unsigned *lines, unsigned *blocks)
{
    *enabled = (v & (1u << 31)) != 0;
    *lines = (v >> 14) & 0x1FFFu;
    *blocks = v & 0x1FFFu;
}
