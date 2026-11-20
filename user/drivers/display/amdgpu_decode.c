/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What an AMD Southern Islands GPU's registers say, and its DMA engines'
 * commands (`amdgpu_decode.h`, `docs/amdgpu.md`). Pure: no hardware and no
 * system calls, so `tools/test_amdgpu_decode.c` holds it on the Mac.
 */

#include "amdgpu_decode.h"

/*
 * **Every Southern Islands device id**, from Linux v6.12's
 * `include/drm/drm_pciids.h` (radeon's table, 73 of them), which amdgpu's
 * `amdgpu_drv.c` repeats but for 0x684c. Generated from that file rather
 * than typed, in order.
 */
static const struct {
    uint16_t device;
    uint8_t  family;
} SI_IDS[] = {
    { 0x6600u, AMDGPU_OLAND },
    { 0x6601u, AMDGPU_OLAND },
    { 0x6602u, AMDGPU_OLAND },
    { 0x6603u, AMDGPU_OLAND },
    { 0x6604u, AMDGPU_OLAND },
    { 0x6605u, AMDGPU_OLAND },
    { 0x6606u, AMDGPU_OLAND },
    { 0x6607u, AMDGPU_OLAND },
    { 0x6608u, AMDGPU_OLAND },
    { 0x6610u, AMDGPU_OLAND },
    { 0x6611u, AMDGPU_OLAND },
    { 0x6613u, AMDGPU_OLAND },
    { 0x6617u, AMDGPU_OLAND },
    { 0x6620u, AMDGPU_OLAND },
    { 0x6621u, AMDGPU_OLAND },
    { 0x6623u, AMDGPU_OLAND },
    { 0x6631u, AMDGPU_OLAND },
    { 0x6660u, AMDGPU_HAINAN },
    { 0x6663u, AMDGPU_HAINAN },
    { 0x6664u, AMDGPU_HAINAN },
    { 0x6665u, AMDGPU_HAINAN },
    { 0x6667u, AMDGPU_HAINAN },
    { 0x666fu, AMDGPU_HAINAN },
    { 0x6780u, AMDGPU_TAHITI },
    { 0x6784u, AMDGPU_TAHITI },
    { 0x6788u, AMDGPU_TAHITI },
    { 0x678au, AMDGPU_TAHITI },
    { 0x6790u, AMDGPU_TAHITI },
    { 0x6791u, AMDGPU_TAHITI },
    { 0x6792u, AMDGPU_TAHITI },
    { 0x6798u, AMDGPU_TAHITI },
    { 0x6799u, AMDGPU_TAHITI },
    { 0x679au, AMDGPU_TAHITI },
    { 0x679bu, AMDGPU_TAHITI },
    { 0x679eu, AMDGPU_TAHITI },
    { 0x679fu, AMDGPU_TAHITI },
    { 0x6800u, AMDGPU_PITCAIRN },
    { 0x6801u, AMDGPU_PITCAIRN },
    { 0x6802u, AMDGPU_PITCAIRN },
    { 0x6806u, AMDGPU_PITCAIRN },
    { 0x6808u, AMDGPU_PITCAIRN },
    { 0x6809u, AMDGPU_PITCAIRN },
    { 0x6810u, AMDGPU_PITCAIRN },
    { 0x6811u, AMDGPU_PITCAIRN },
    { 0x6816u, AMDGPU_PITCAIRN },
    { 0x6817u, AMDGPU_PITCAIRN },
    { 0x6818u, AMDGPU_PITCAIRN },
    { 0x6819u, AMDGPU_PITCAIRN },
    { 0x6820u, AMDGPU_VERDE },
    { 0x6821u, AMDGPU_VERDE },
    { 0x6822u, AMDGPU_VERDE },
    { 0x6823u, AMDGPU_VERDE },
    { 0x6824u, AMDGPU_VERDE },
    { 0x6825u, AMDGPU_VERDE },
    { 0x6826u, AMDGPU_VERDE },
    { 0x6827u, AMDGPU_VERDE },
    { 0x6828u, AMDGPU_VERDE },
    { 0x6829u, AMDGPU_VERDE },
    { 0x682au, AMDGPU_VERDE },
    { 0x682bu, AMDGPU_VERDE },
    { 0x682cu, AMDGPU_VERDE },
    { 0x682du, AMDGPU_VERDE },
    { 0x682fu, AMDGPU_VERDE },
    { 0x6830u, AMDGPU_VERDE },
    { 0x6831u, AMDGPU_VERDE },
    { 0x6835u, AMDGPU_VERDE },
    { 0x6837u, AMDGPU_VERDE },
    { 0x6838u, AMDGPU_VERDE },
    { 0x6839u, AMDGPU_VERDE },
    { 0x683bu, AMDGPU_VERDE },
    { 0x683du, AMDGPU_VERDE },
    { 0x683fu, AMDGPU_VERDE },
    { 0x684cu, AMDGPU_PITCAIRN },
};

/*
 * Each family's shape, from `si.c`'s `si_gpu_init` (max_shader_engines,
 * max_sh_per_se, max_cu_per_sh) and `radeon_asic.c`'s num_crtc: Hainan has
 * no display at all, Oland two controllers, the rest six.
 */
static const struct amdgpu_family_info FAMILIES[] = {
    [AMDGPU_NOT_SI]   = { "not Southern Islands", 0, 0, 0, 0 },
    [AMDGPU_TAHITI]   = { "Tahiti",   2, 2, 8, 6 },
    [AMDGPU_PITCAIRN] = { "Pitcairn", 2, 2, 5, 6 },
    [AMDGPU_VERDE]    = { "Cape Verde", 1, 2, 5, 6 },
    [AMDGPU_OLAND]    = { "Oland",    1, 1, 6, 2 },
    [AMDGPU_HAINAN]   = { "Hainan",   1, 1, 5, 0 },
};

enum amdgpu_family amdgpu_family_of(uint16_t device)
{
    size_t i;

    for (i = 0; i < sizeof SI_IDS / sizeof SI_IDS[0]; i++) {
        if (SI_IDS[i].device == device) {
            return (enum amdgpu_family)SI_IDS[i].family;
        }
    }

    return AMDGPU_NOT_SI;
}

const struct amdgpu_family_info *amdgpu_family_info(enum amdgpu_family family)
{
    if ((unsigned)family >= sizeof FAMILIES / sizeof FAMILIES[0]) {
        family = AMDGPU_NOT_SI;
    }

    return &FAMILIES[family];
}

/*
 * The Mac Pro 2013's cards, by the PCI ID Repository (`pci.ids`, fetched
 * 20 November 2026): under 1002:6798 "Tahiti XT", subsystems 106b:0127 and
 * 106b:0128 "FirePro D700"; under 1002:679e "Tahiti LE", 106b:0125 and
 * 106b:0126 "FirePro D500"; under 1002:6810 "Curacao XT / Trinidad XT",
 * which `drm_pciids.h` calls Pitcairn, 106b:012a and 106b:012b "FirePro
 * D300". Two subsystem ids each - one for each of the machine's two cards.
 */
static const struct {
    uint16_t    device;
    uint16_t    subsystem_id;
    const char *name;
} CARDS[] = {
    { 0x6798u, 0x0127u, "FirePro D700" },
    { 0x6798u, 0x0128u, "FirePro D700" },
    { 0x679eu, 0x0125u, "FirePro D500" },
    { 0x679eu, 0x0126u, "FirePro D500" },
    { 0x6810u, 0x012au, "FirePro D300" },
    { 0x6810u, 0x012bu, "FirePro D300" },
};

const char *amdgpu_card_name(uint16_t device, uint32_t subsystem)
{
    size_t i;

    if ((subsystem & 0xffffu) != AMDGPU_APPLE) {
        return NULL;
    }

    for (i = 0; i < sizeof CARDS / sizeof CARDS[0]; i++) {
        if (CARDS[i].device == device && CARDS[i].subsystem_id == (subsystem >> 16)) {
            return CARDS[i].name;
        }
    }

    return NULL;
}

/* `si_mc_init`: "some boards may have garbage in the upper 16 bits". */
uint32_t amdgpu_vram_mb(uint32_t config_memsize)
{
    if ((config_memsize & 0xffff0000u) != 0 && (config_memsize & 0xffffu) != 0) {
        return config_memsize & 0xffffu;
    }

    return config_memsize;
}

/* `si_mc_init`: the channel's width, and how many channels (NOOFCHAN). */
unsigned amdgpu_vram_bits(uint32_t ramcfg, uint32_t chmap)
{
    static const unsigned CHANNELS[16] = { 1, 2, 4, 8, 3, 6, 10, 12, 16,
                                           1, 1, 1, 1, 1, 1, 1 };
    unsigned size = (ramcfg & (1u << 11)) ? 16u : (ramcfg & (1u << 8)) ? 64u : 32u;

    return size * CHANNELS[(chmap >> 12) & 0xfu];
}

/* MC_VM_FB_LOCATION: `si_mc_program` writes vram_start >> 24 in 15:0 and
 * vram_end >> 24 - the last byte's - in 31:16. */
void amdgpu_fb_location(uint32_t fb_location, uint64_t *start, uint64_t *end)
{
    *start = (uint64_t)(fb_location & 0xffffu) << 24;
    *end = ((uint64_t)(fb_location >> 16) << 24) | 0xffffffu;
}

bool amdgpu_mc_to_bar(uint64_t mc, uint32_t hdp_nonsurface_base, uint64_t aperture,
                      uint64_t *offset)
{
    uint64_t base = (uint64_t)hdp_nonsurface_base << 8;

    if (mc < base || mc - base >= aperture) {
        return false;
    }

    *offset = mc - base;
    return true;
}

/* `si_get_cu_active_bitmap`: the fused and the chosen inactive bits, 31:16
 * of each, ored; what is left of cu_per_sh's bits is active. */
unsigned amdgpu_cus_active(uint32_t cc_shader_array, uint32_t user_shader_array,
                           unsigned cu_per_sh)
{
    uint32_t inactive = ((cc_shader_array & 0xffff0000u) | user_shader_array) >> 16;
    uint32_t mask = cu_per_sh >= 32 ? 0xffffffffu : (1u << cu_per_sh) - 1u;
    uint32_t active = ~inactive & mask;
    unsigned n = 0;

    while (active != 0) {
        n += active & 1u;
        active >>= 1;
    }

    return n;
}

/* `si_get_temp`: CTF_TEMP in 17:9. Its `& 0x200` test is kept as Linux has
 * it, though under a nine-bit mask it can never be true. */
unsigned amdgpu_temperature(uint32_t status)
{
    uint32_t t = (status & 0x0003fe00u) >> 9;

    return (t & 0x200u) ? 255u : (unsigned)(t & 0x1ffu);
}

uint32_t amdgpu_crtc_offset(unsigned crtc)
{
    static const uint32_t OFFSETS[AMDGPU_CRTCS_MAX] = {
        0x6df0u - 0x6df0u, 0x79f0u - 0x6df0u, 0x105f0u - 0x6df0u,
        0x111f0u - 0x6df0u, 0x11df0u - 0x6df0u, 0x129f0u - 0x6df0u,
    };

    return crtc < AMDGPU_CRTCS_MAX ? OFFSETS[crtc] : 0u;
}

void amdgpu_decode_grph(const struct amdgpu_grph_regs *r, struct amdgpu_grph *out)
{
    static const unsigned BPP[4] = { 8, 16, 32, 64 };

    out->crtc_on = (r->crtc_control & 1u) != 0;
    out->enabled = (r->grph_enable & 1u) != 0;
    out->bpp = BPP[r->grph_control & 0x3u];
    out->format = (r->grph_control >> 8) & 0x7u;
    out->array_mode = (r->grph_control >> 20) & 0x7u;
    out->linear = out->array_mode <= 1u;
    out->address = ((uint64_t)(r->primary_high & 0xffu) << 32) | (r->primary & 0xffffff00u);
    out->pitch_px = r->pitch & 0x7fffu;
    out->width = r->x_end > r->x_start ? r->x_end - r->x_start : 0u;
    out->height = r->y_end > r->y_start ? r->y_end - r->y_start : 0u;
    out->view_w = (r->viewport_size >> 16) & 0x3fffu;
    out->view_h = r->viewport_size & 0x3fffu;
    out->pending = (r->update & AMDGPU_GRPH_UPDATE_PENDING) != 0;
}

const char *amdgpu_array_mode_name(unsigned mode)
{
    switch (mode) {
    case 0: return "linear";
    case 1: return "linear aligned";
    case 2: return "1D tiled";
    case 4: return "2D tiled";
    default: return "another tiling";
    }
}

/* DMA_PACKET(cmd, b, t, s, n): cmd 31:28, b 26, t 23, s 22, n 19:0. */
uint32_t amdgpu_dma_header(unsigned cmd, unsigned b, unsigned t, unsigned s, uint32_t n)
{
    return ((uint32_t)(cmd & 0xfu) << 28) | ((uint32_t)(b & 1u) << 26)
         | ((uint32_t)(t & 1u) << 23) | ((uint32_t)(s & 1u) << 22) | (n & 0xfffffu);
}

/* `si_dma_ring_test_ring`: WRITE, one dword - the address low (dword
 * aligned) and high byte, and the value. */
unsigned amdgpu_dma_write(uint32_t *out, uint64_t addr, uint32_t value)
{
    out[0] = amdgpu_dma_header(AMDGPU_DMA_WRITE, 0, 0, 0, 1);
    out[1] = (uint32_t)addr & 0xfffffffcu;
    out[2] = (uint32_t)(addr >> 32) & 0xffu;
    out[3] = value;
    return 4;
}

/* `si_dma_emit_fill_buffer`: CONSTANT_FILL, its count in dwords, the high
 * address byte in 23:16 of the last word. */
unsigned amdgpu_dma_fill(uint32_t *out, uint64_t addr, uint32_t value, uint32_t bytes)
{
    out[0] = amdgpu_dma_header(AMDGPU_DMA_FILL, 0, 0, 0, bytes / 4u);
    out[1] = (uint32_t)addr;
    out[2] = value;
    out[3] = (uint32_t)(addr >> 32) << 16;
    return 4;
}

/* `si_copy_dma` (radeon) and `si_dma_emit_copy_buffer`: COPY with b set,
 * bytes; the destination's low word before the source's, then each high byte. */
unsigned amdgpu_dma_copy(uint32_t *out, uint64_t dst, uint64_t src, uint32_t bytes)
{
    out[0] = amdgpu_dma_header(AMDGPU_DMA_COPY, 1, 0, 0, bytes);
    out[1] = (uint32_t)dst;
    out[2] = (uint32_t)src;
    out[3] = (uint32_t)(dst >> 32) & 0xffu;
    out[4] = (uint32_t)(src >> 32) & 0xffu;
    return 5;
}

/* `si_dma_ring_emit_fence`, its 32-bit half, without the trap: nothing
 * here listens for an interrupt yet. */
unsigned amdgpu_dma_fence(uint32_t *out, uint64_t addr, uint32_t seq)
{
    out[0] = amdgpu_dma_header(AMDGPU_DMA_FENCE, 0, 0, 0, 0);
    out[1] = (uint32_t)addr & 0xfffffffcu;
    out[2] = (uint32_t)(addr >> 32) & 0xffu;
    out[3] = seq;
    return 4;
}

unsigned amdgpu_dma_nop(uint32_t *out)
{
    out[0] = amdgpu_dma_header(AMDGPU_DMA_NOP, 0, 0, 0, 0);
    return 1;
}

/* `si_dma_start`: rb_bufsz = order_base_2(ring_size / 4), rb_cntl = it << 1. */
uint32_t amdgpu_dma_rb_size(uint32_t bytes)
{
    uint32_t dwords = bytes / 4u, log2 = 0;

    while ((1u << log2) < dwords && log2 < 31u) {
        log2++;
    }

    return log2 << 1;
}

/* `si_dma_ring_set_wptr`: (wptr << 2) & 0x3fffc. */
uint32_t amdgpu_dma_wptr(uint32_t dwords)
{
    return (dwords << 2) & 0x3fffcu;
}
