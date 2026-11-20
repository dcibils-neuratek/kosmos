/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `amdgpu_decode.c` held to numbers, on the Mac: QEMU has no AMD GPU, so the
 * Mac Pro's display driver reads only there (`docs/amdgpu.md`). Each value
 * is built from the field layout Linux's headers give - the way a firmware
 * or Linux itself would have written it - and each control is what a wrong
 * mask, shift or table would give instead.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O1 -o build/host/test_amdgpu_decode \
 *      tools/test_amdgpu_decode.c user/drivers/display/amdgpu_decode.c
 */

#include <stdio.h>
#include <string.h>

#include "../user/drivers/display/amdgpu_decode.h"

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
    uint64_t start, end, off;
    struct amdgpu_grph_regs r;
    struct amdgpu_grph g;
    uint32_t w[8];
    const struct amdgpu_family_info *f;

    /* The Mac Pro's three, by device id (`drm_pciids.h`). */
    check(amdgpu_family_of(0x6798) == AMDGPU_TAHITI, "6798 is Tahiti (D700)");
    check(amdgpu_family_of(0x679e) == AMDGPU_TAHITI, "679e is Tahiti (D500)");
    check(amdgpu_family_of(0x6810) == AMDGPU_PITCAIRN, "6810 is Pitcairn (D300)");
    check(amdgpu_family_of(0x684c) == AMDGPU_PITCAIRN, "684c, radeon's alone, is Pitcairn");
    check(amdgpu_family_of(0x6600) == AMDGPU_OLAND && amdgpu_family_of(0x666f) == AMDGPU_HAINAN
          && amdgpu_family_of(0x683f) == AMDGPU_VERDE, "the table's other families");
    /* Controls: Bonaire (Sea Islands) and an Intel id are not SI. */
    check(amdgpu_family_of(0x6650) == AMDGPU_NOT_SI, "control: 6650 is Bonaire, not SI");
    check(amdgpu_family_of(0x1912) == AMDGPU_NOT_SI, "control: an HD 530's id is nothing here");

    f = amdgpu_family_info(AMDGPU_TAHITI);
    check(f->shader_engines * f->sh_per_se * f->cu_per_sh == 32 && f->crtcs == 6,
          "Tahiti: 2 x 2 x 8 = 32 compute units at most, six controllers");
    f = amdgpu_family_info(AMDGPU_PITCAIRN);
    check(f->shader_engines * f->sh_per_se * f->cu_per_sh == 20, "Pitcairn: 2 x 2 x 5 = 20");
    check(amdgpu_family_info(AMDGPU_HAINAN)->crtcs == 0 && amdgpu_family_info(AMDGPU_OLAND)->crtcs == 2,
          "Hainan no display, Oland two");
    check(strcmp(amdgpu_family_info((enum amdgpu_family)99)->name, "not Southern Islands") == 0,
          "a family past the table is none");

    /* Subsystem word: vendor 15:0, id 31:16 (configuration offset 0x2c). */
    check(amdgpu_card_name(0x6798, 0x0127106bu) && strcmp(amdgpu_card_name(0x6798, 0x0127106bu), "FirePro D700") == 0,
          "106b:0127 under 6798 is a D700");
    check(amdgpu_card_name(0x679e, 0x0126106bu) && strcmp(amdgpu_card_name(0x679e, 0x0126106bu), "FirePro D500") == 0,
          "106b:0126 under 679e is a D500");
    check(amdgpu_card_name(0x6810, 0x012b106bu) && strcmp(amdgpu_card_name(0x6810, 0x012b106bu), "FirePro D300") == 0,
          "106b:012b under 6810 is a D300");
    check(amdgpu_card_name(0x6798, 0x106b0127u) == NULL, "control: the halves swapped name nothing");
    check(amdgpu_card_name(0x6810, 0x0127106bu) == NULL, "control: a D700's subsystem under 6810 is nothing");

    /* Memory: CONFIG_MEMSIZE in MB, garbage high handled. */
    check(amdgpu_vram_mb(6144) == 6144 && amdgpu_vram_mb(0x12340800u) == 0x800,
          "6 GB, and 2 GB under garbage in 31:16");
    check(amdgpu_vram_mb(0x10000u) == 0x10000u, "high bits alone are kept, as si_mc_init does");
    /* 384 bits: twelve channels (NOOFCHAN 7) of 32 (CHANSIZE clear). */
    check(amdgpu_vram_bits(0, 7u << 12) == 384, "Tahiti's 384-bit bus");
    check(amdgpu_vram_bits(0, 3u << 12) == 256, "eight channels of 32: 256 bits");
    check(amdgpu_vram_bits(1u << 8, 3u << 12) == 512, "CHANSIZE set: 64-bit channels");
    check(amdgpu_vram_bits((1u << 8) | (1u << 11), 3u << 12) == 128, "the override wins: 16-bit");
    check(amdgpu_vram_bits(0, 7u) == 32, "control: NOOFCHAN read from 3:0 would be one channel");

    /* MC_VM_FB_LOCATION as si_mc_program writes it for 6 GB at 0xF400000000. */
    {
        uint64_t s = 0xf400000000ull, e = s + 6144ull * 1024 * 1024 - 1;
        uint32_t v = (uint32_t)(((e >> 24) & 0xffff) << 16) | (uint32_t)((s >> 24) & 0xffff);

        amdgpu_fb_location(v, &start, &end);
        check(start == s && end == e, "FB_LOCATION round trip, end inclusive");
    }
    amdgpu_fb_location(0x00ff0000u, &start, &end);
    check(start == 0 && end == 0xffffffffull, "4 GB from nought");

    /* BAR 0's window: HDP_NONSURFACE_BASE is vram_start >> 8. */
    check(amdgpu_mc_to_bar(0xf400100000ull, (uint32_t)(0xf400000000ull >> 8), 256u << 20, &off)
          && off == 0x100000, "an MC address 1 MB in is 1 MB into the BAR");
    check(!amdgpu_mc_to_bar(0xf400000000ull + (256ull << 20), (uint32_t)(0xf400000000ull >> 8),
                            256u << 20, &off), "the first byte past the aperture is outside");
    check(!amdgpu_mc_to_bar(0xf3ffffff00ull, (uint32_t)(0xf400000000ull >> 8), 256u << 20, &off),
          "below the base is outside");

    /* Compute units: 31:16 inactive, of each register. */
    check(amdgpu_cus_active(0, 0, 8) == 8, "all eight of a Tahiti array on");
    check(amdgpu_cus_active(0x00030000u, 0, 8) == 6, "two fused off: a D500's array");
    check(amdgpu_cus_active(0, 0x00800000u, 8) == 7, "one chosen off");
    check(amdgpu_cus_active(0x0000ffffu, 0, 5) == 5, "control: the low half of CC is not inactive bits");

    /* CTF_TEMP_MASK is 17:9, nine bits, so si_get_temp's `temp & 0x200` -
     * the tenth - can never be set: the largest reading is 511. */
    check(amdgpu_temperature(45u << 9) == 45 && amdgpu_temperature(0x3ffu << 9) == 511,
          "CTF_TEMP 17:9, and the tenth bit outside the mask");
    check(amdgpu_temperature(45u) == 0, "control: degrees are not in 8:0");

    check(amdgpu_crtc_offset(0) == 0 && amdgpu_crtc_offset(1) == 0xc00 && amdgpu_crtc_offset(2) == 0x9800
          && amdgpu_crtc_offset(5) == 0xbc00, "controller offsets, evergreen_reg.h");
    check(amdgpu_crtc_offset(1) == 0x300 * 4 && amdgpu_crtc_offset(3) == 0x2900 * 4,
          "and amdgpu's sid.h dwords agree");
    check(AMDGPU_GRPH_PITCH == 0x1a06u * 4 && AMDGPU_CRTC_CONTROL == 0x1b9cu * 4
          && AMDGPU_VIEWPORT_SIZE == 0x1b5du * 4 && AMDGPU_CRTC_FRAME_COUNT == 0x1ba6u * 4
          && AMDGPU_GRPH_PRIMARY_HIGH == 0x1a07u * 4 && AMDGPU_D1VGA_CONTROL == 0x00ccu * 4,
          "evergreen_reg.h's bytes are dce_6_0_d.h's dwords");
    check(AMDGPU_CONFIG_MEMSIZE == 0x150au * 4 && AMDGPU_HDP_MEM_FLUSH == 0x1520u * 4
          && AMDGPU_CONFIG_APER_SIZE == 0x150cu * 4 && AMDGPU_CC_DRM_ID_STRAPS == 0x1559u * 4
          && AMDGPU_HDP_DEBUG0 == 0x0bccu * 4 && AMDGPU_DMA_RB_RPTR == 0x3402u * 4
          && AMDGPU_DMA_SEM_INCOMPLETE == 0x3411u * 4,
          "sid.h's bytes are bif/oss/amdgpu sid.h's dwords");
    check(AMDGPU_CRTC_FRAME_COUNT + amdgpu_crtc_offset(5) < AMDGPU_REGS_BYTES
          && AMDGPU_DMA_PG + AMDGPU_DMA1_OFFSET < AMDGPU_REGS_BYTES,
          "every register used is inside the mapping");

    /* A firmware's screen: 32 bpp ARGB8888 linear, 2560x1440, pitch 2560. */
    memset(&r, 0, sizeof r);
    r.crtc_control = 1u | (1u << 24) * 0;
    r.grph_enable = 1;
    r.grph_control = 2u | (0u << 8) | (0u << 20);
    r.primary = 0x00100000u | 0x1u;         /* DFQ in bit 0 is not address */
    r.primary_high = 0xf4u;
    r.pitch = 2560;
    r.x_end = 2560;
    r.y_end = 1440;
    r.viewport_size = (2560u << 16) | 1440u;
    amdgpu_decode_grph(&r, &g);
    check(g.crtc_on && g.enabled && g.bpp == 32 && g.linear, "on, 32 bpp, linear");
    check(g.address == 0xf400100000ull, "the address: 39:32 high, 31:8 low");
    check(g.width == 2560 && g.height == 1440 && g.view_w == 2560 && g.view_h == 1440 && g.pitch_px == 2560,
          "the surface's and the viewport's sizes");
    check(!g.pending, "no flip pending");
    r.grph_control = 2u | (4u << 20);
    r.update = AMDGPU_GRPH_UPDATE_PENDING;
    amdgpu_decode_grph(&r, &g);
    check(!g.linear && strcmp(amdgpu_array_mode_name(g.array_mode), "2D tiled") == 0 && g.pending,
          "2D tiled, and a flip pending");
    r.grph_control = 1u;
    amdgpu_decode_grph(&r, &g);
    check(g.bpp == 16, "control: DEPTH 1 is 16 bpp");

    /* DMA commands, against `sid.h`'s DMA_PACKET by hand. */
    check(amdgpu_dma_header(AMDGPU_DMA_WRITE, 0, 0, 0, 1) == 0x20000001u, "WRITE of one dword");
    check(amdgpu_dma_header(AMDGPU_DMA_COPY, 1, 0, 0, 0x1000) == 0x34001000u, "COPY in bytes: b in 26");
    check(amdgpu_dma_header(AMDGPU_DMA_NOP, 0, 0, 0, 0) == 0xf0000000u, "NOP");
    check(amdgpu_dma_header(AMDGPU_DMA_FILL, 0, 0, 0, 0x100000) == 0xd0000000u,
          "control: n is twenty bits, the twenty-first lost");

    check(amdgpu_dma_write(w, 0xf400200007ull, 0xdeadbeefu) == 4 && w[0] == 0x20000001u
          && w[1] == 0x00200004u && w[2] == 0xf4u && w[3] == 0xdeadbeefu,
          "write: address dword aligned, high byte, value");
    check(amdgpu_dma_fill(w, 0xf400300000ull, 0x11223344u, 4096) == 4 && w[0] == 0xd0000400u
          && w[1] == 0x00300000u && w[2] == 0x11223344u && w[3] == 0x00f40000u,
          "fill: a count of dwords, the high byte in 23:16");
    check(amdgpu_dma_copy(w, 0xf400400000ull, 0xf400100000ull, 0x2000) == 5 && w[0] == 0x34002000u
          && w[1] == 0x00400000u && w[2] == 0x00100000u && w[3] == 0xf4u && w[4] == 0xf4u,
          "copy: destination before source");
    check(amdgpu_dma_fence(w, 0xf400200010ull, 7) == 4 && w[0] == 0x60000000u && w[3] == 7,
          "fence");
    check(amdgpu_dma_nop(w) == 1 && w[0] == 0xf0000000u, "nop");

    check(amdgpu_dma_rb_size(16384) == (12u << 1) && amdgpu_dma_rb_size(65536) == (14u << 1),
          "ring size: log2 dwords << 1");
    check(amdgpu_dma_wptr(4) == 16 && amdgpu_dma_wptr(0x10000) == 0, "wptr: dwords << 2, 17:2");

    printf("amdgpu_decode: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
