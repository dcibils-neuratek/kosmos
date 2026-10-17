/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `intelgpu_decode.c` held to numbers, on the Mac: QEMU has no Intel GPU, so
 * the M700's display driver reads only there (`docs/m700-2d.md` step 2).
 * Each value is built from the field layout `intelgpu_decode.h` cites from
 * Linux's i915, the way a firmware would have set it - and the controls are
 * the ones a wrong mask or shift would give.
 */

#include <stdio.h>
#include <string.h>

#include "../user/drivers/display/intelgpu_decode.h"

static int checks, failed;

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        failed++;
        printf("FAIL: %s\n", what);
    }
}

int main(void)
{
    struct igpu_plane p;
    struct igpu_plane_regs r;
    unsigned w, h;
    int x, y;
    uint64_t phys;

    /* A firmware's screen: plane 1 on, XRGB8888, X-tiled, 3440x1440 at
     * 0,0, 3456 * 4 bytes a row = 27 tiles of 512, surface at 0x00000000. */
    memset(&r, 0, sizeof r);
    r.ctl = (1u << 31) | (4u << 24) | (1u << 10);
    r.stride = 27;
    r.size = ((1440u - 1) << 16) | (3440u - 1);
    r.surf = 0x00000000u;
    igpu_decode_plane(&r, &p);
    check(p.enabled && p.format && strcmp(p.format, "XRGB8888") == 0,
          "plane on, XRGB8888");
    check(p.tiling && strcmp(p.tiling, "X") == 0, "tiling X");
    check(p.width == 3440 && p.height == 1440, "size 3440x1440 from SIZE's height-1 and width-1");
    check(p.stride_bytes == 27 * 512, "an X-tiled stride counts tiles of 512 bytes");

    /* Linear: units of 64 bytes; a surface with low bits set keeps 31:12. */
    r.ctl = (1u << 31) | (4u << 24);
    r.stride = 7168 / 64;                   /* 1720 wide, rows padded to 7168 */
    r.surf = 0x00ABC123u;
    r.pos = (12u << 16) | 34u;
    igpu_decode_plane(&r, &p);
    check(p.tiling && strcmp(p.tiling, "linear") == 0 && p.stride_bytes == 7168,
          "a linear stride counts 64-byte chunks");
    check(p.surface == 0x00ABC000u, "the surface keeps bits 31:12");
    check(p.x == 34 && p.y == 12, "position: y high, x low");

    /* Y-tiled: 128 bytes a tile's width. */
    r.ctl = (1u << 31) | (4u << 24) | (4u << 10);
    r.stride = 10;
    igpu_decode_plane(&r, &p);
    check(p.tiling && strcmp(p.tiling, "Y") == 0 && p.stride_bytes == 1280, "Y tiles of 128 bytes");

    /* Controls: off is off; a format not named says so. */
    r.ctl = 4u << 24;
    igpu_decode_plane(&r, &p);
    check(!p.enabled, "bit 31 clear is a plane off");
    r.ctl = (1u << 31) | (9u << 24);
    igpu_decode_plane(&r, &p);
    check(p.format == NULL && p.format_code == 9, "an unnamed format is its number");

    igpu_decode_pipesrc(((1720u - 1) << 16) | (1440u - 1), &w, &h);
    check(w == 1720 && h == 1440, "PIPESRC: width-1 high, height-1 low");

    check(igpu_cursor_mode(0) == NULL, "a cursor's mode 0 is off");
    check(igpu_cursor_mode(0x27) && strcmp(igpu_cursor_mode(0x27), "64x64 ARGB") == 0,
          "mode 0x27 is 64x64 ARGB");
    check(igpu_cursor_mode((1u << 26) | 0x27) && strcmp(igpu_cursor_mode((1u << 26) | 0x27), "64x64 ARGB") == 0,
          "the mode ignores the gamma bit beside it");

    igpu_decode_curpos((1u << 31) | (5u << 16) | (1u << 15) | 7u, &x, &y);
    check(x == -7 && y == -5, "CURPOS: each coordinate signed by its own bit");

    check(igpu_decode_gtt(0x0000000123456003ull, &phys) && phys == 0x123456000ull,
          "a GTT entry: present, its page in 45:12");
    check(!igpu_decode_gtt(0x0000000123456000ull, &phys), "bit 0 clear is not present");
    check(igpu_decode_gtt(0xFFFF000000001001ull, &phys) && phys == 0x1000ull,
          "bits above 45 are not the address");

    printf("%s: %d checks on intelgpu_decode\n", failed ? "FAIL" : "PASS", checks);
    return failed != 0;
}
