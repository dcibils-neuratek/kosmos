/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_DISPLAY_AMDGPU_DECODE_H
#define KOSMOS_DRIVERS_DISPLAY_AMDGPU_DECODE_H

/*
 * What an AMD Southern Islands GPU's registers say - which chip it is, how
 * much memory it has and where the memory controller puts it, what each of
 * the six display controllers scans out - and the commands its two DMA
 * engines take, for the Mac Pro 2013's FirePro D300, D500 and D700
 * (`docs/amdgpu.md`).
 *
 * Its own file, with no hardware and no system calls in it, as
 * `intelgpu_decode.c` is: QEMU has no AMD GPU, so this is held to numbers on
 * the Mac (`tools/test_amdgpu_decode.c`) and to the Mac Pro by what the
 * driver says there.
 *
 * **Every offset and field is Linux's**, read from the v6.12 tree in
 * `build/downloads/linux-amd/` and checked twice where two headers give it:
 * radeon's `sid.h` and `evergreen_reg.h` in bytes, and amdgpu's
 * `asic_reg/dce/dce_6_0_d.h`, `gmc_6_0_d.h`, `bif_3_0_d.h` and
 * `oss_1_0_d.h` in dwords (an `mm` name times four is the byte offset).
 * `docs/amdgpu.md` lists each register with the file and line it is from.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AMDGPU_VENDOR              0x1002u
#define AMDGPU_APPLE               0x106bu     /* a subsystem vendor */

/*
 * The register BAR. Southern Islands' registers are BAR 2 (`radeon_device.c`
 * and `amdgpu_device.c`: BAR 5 only from Bonaire, which is Sea Islands);
 * BAR 0 is the VRAM aperture. The driver maps the first AMDGPU_REGS_BYTES of
 * BAR 2: every register it touches is below it, the highest the sixth
 * display controller's frame count at 0x6e98 + 0xbc00 = 0x12a98. Linux maps
 * the BAR's whole length (`pci_resource_len`), which every SI card reports
 * as 256 KB - from lspci listings, not from an AMD document - so asking for
 * 76 KB of it is asking for less than any SI card has.
 */
#define AMDGPU_REGS_BYTES          0x13000u

/* Chip and memory controller (`sid.h`; `bif_3_0_d.h`, `oss_1_0_d.h`). */
#define AMDGPU_SRBM_STATUS         0x0e50u
#define AMDGPU_SRBM_SOFT_RESET     0x0e60u
#define   AMDGPU_SOFT_RESET_DMA1   (1u << 6)
#define   AMDGPU_SOFT_RESET_DMA    (1u << 20)
#define AMDGPU_SRBM_STATUS2        0x0ec4u
#define   AMDGPU_DMA_BUSY          (1u << 5)
#define   AMDGPU_DMA1_BUSY         (1u << 6)
#define AMDGPU_VM_L2_CNTL          0x1400u
#define AMDGPU_VM_CONTEXT0_CNTL    0x1410u
#define   AMDGPU_ENABLE_CONTEXT    (1u << 0)
#define AMDGPU_MC_SHARED_CHMAP     0x2004u     /* NOOFCHAN 15:12 */
#define AMDGPU_MC_VM_FB_LOCATION   0x2024u     /* base 15:0, top 31:16, 16 MB units */
#define AMDGPU_MC_VM_SYS_APER_LOW  0x2034u     /* 4 KB units */
#define AMDGPU_MC_VM_SYS_APER_HIGH 0x2038u
#define AMDGPU_MC_VM_MX_L1_TLB_CNTL 0x2064u    /* ENABLE_L1_TLB 0, SYSTEM_ACCESS_MODE 4:3 */
#define AMDGPU_MC_ARB_RAMCFG       0x2760u     /* CHANSIZE 8, CHANSIZE_OVERRIDE 11 */
#define AMDGPU_MC_SEQ_SUP_CNTL     0x28c8u     /* RUN_MASK 0: the MC's own engine runs */
#define AMDGPU_MC_SEQ_MISC0        0x2a00u     /* the memory's vendor 11:8, GDDR5 31:28 == 5 */
#define AMDGPU_HDP_NONSURFACE_BASE 0x2c04u     /* the MC address BAR 0 begins at, >> 8 */
#define AMDGPU_HDP_DEBUG0          0x2f30u     /* mmHDP_DEBUG0 0x0bcc: written 1, the read cache let go */
#define AMDGPU_CONFIG_MEMSIZE      0x5428u     /* VRAM, in MB on SI */
#define AMDGPU_CONFIG_APER_SIZE    0x5430u     /* mmCONFIG_APER_SIZE 0x150c */
#define AMDGPU_HDP_MEM_FLUSH       0x5480u     /* HDP_MEM_COHERENCY_FLUSH_CNTL: written 1, writes reach memory */
#define AMDGPU_CC_DRM_ID_STRAPS    0x5564u     /* mmCC_DRM_ID_STRAPS 0x1559: the device id in 19:4 */
#define AMDGPU_CG_MULT_THERMAL     0x0714u     /* CTF_TEMP 17:9, degrees */

/* The graphics engine, read for its shape (`sid.h`). */
#define AMDGPU_GRBM_STATUS         0x8010u
#define AMDGPU_GRBM_GFX_INDEX      0x802cu
#define AMDGPU_GFX_BROADCAST       0xe0000000u /* SE, SH and instance broadcast: as Linux leaves it */
#define AMDGPU_CP_ME_CNTL          0x86d8u     /* halts: CE 24, PFP 26, ME 28 */
#define AMDGPU_CC_GC_SHADER_ARRAY  0x89bcu     /* INACTIVE_CUS 31:16, fused */
#define AMDGPU_GC_USER_SHADER_ARRAY 0x89c0u    /* the same, chosen by a driver */
#define AMDGPU_RLC_CNTL            0xc300u

/* The DMA engines: the first at 0xd000, the second 0x800 on (`sid.h`). */
#define AMDGPU_DMA_ENGINES         2u
#define AMDGPU_DMA1_OFFSET         0x800u
#define AMDGPU_DMA_RB_CNTL         0xd000u
#define   AMDGPU_DMA_RB_ENABLE     (1u << 0)
#define   AMDGPU_DMA_RPTR_WRITEBACK (1u << 12)
#define AMDGPU_DMA_RB_BASE         0xd004u     /* the ring's MC address >> 8 */
#define AMDGPU_DMA_RB_RPTR         0xd008u     /* bytes */
#define AMDGPU_DMA_RB_WPTR         0xd00cu     /* bytes, 17:2 */
#define AMDGPU_DMA_RB_RPTR_ADDR_HI 0xd01cu
#define AMDGPU_DMA_RB_RPTR_ADDR_LO 0xd020u
#define AMDGPU_DMA_IB_CNTL         0xd024u
#define   AMDGPU_DMA_IB_ENABLE     (1u << 0)
#define   AMDGPU_CMD_VMID_FORCE    (1u << 31)  /* amdgpu `sid.h` */
#define AMDGPU_DMA_CNTL            0xd02cu
#define   AMDGPU_CTXEMPTY_INT      (1u << 28)
#define AMDGPU_DMA_STATUS          0xd034u
#define   AMDGPU_DMA_IDLE          (1u << 0)
#define AMDGPU_DMA_SEM_INCOMPLETE  0xd044u     /* r600d.h; amdgpu sid.h 0x3411 */
#define AMDGPU_DMA_SEM_WAIT_FAIL   0xd048u     /*            0x3412 */
#define AMDGPU_DMA_PG              0xd0d4u     /* PG_CNTL_ENABLE 0 */

/*
 * The display controllers (DCE 6): six on Tahiti and Pitcairn, each block
 * the first's offset by `amdgpu_crtc_offset` (`evergreen_reg.h`: CRTC blocks
 * at 0x6df0, 0x79f0, 0x105f0, 0x111f0, 0x11df0, 0x129f0; amdgpu's `sid.h`
 * gives the same as dword offsets 0, 0x300, 0x2600, 0x2900, 0x2c00, 0x2f00).
 */
#define AMDGPU_CRTCS_MAX           6u
#define AMDGPU_VGA_RENDER_CONTROL  0x0300u
#define AMDGPU_D1VGA_CONTROL       0x0330u     /* D2 at 0x338; MODE_ENABLE 0 */
#define AMDGPU_D2VGA_CONTROL       0x0338u
#define AMDGPU_GRPH_ENABLE         0x6800u
#define AMDGPU_GRPH_CONTROL        0x6804u     /* DEPTH 1:0, FORMAT 10:8, ARRAY_MODE 22:20 */
#define AMDGPU_GRPH_SWAP_CONTROL   0x680cu
#define AMDGPU_GRPH_PRIMARY_ADDR   0x6810u     /* 31:8 */
#define AMDGPU_GRPH_PITCH          0x6818u     /* pixels */
#define AMDGPU_GRPH_PRIMARY_HIGH   0x681cu     /* 39:32 */
#define AMDGPU_GRPH_X_START        0x682cu
#define AMDGPU_GRPH_Y_START        0x6830u
#define AMDGPU_GRPH_X_END          0x6834u
#define AMDGPU_GRPH_Y_END          0x6838u
#define AMDGPU_GRPH_UPDATE         0x6844u
#define   AMDGPU_GRPH_UPDATE_PENDING (1u << 2)
#define AMDGPU_GRPH_FLIP_CONTROL   0x6848u     /* H_RETRACE_EN 0: 0 flips at the vertical blank */
#define AMDGPU_VIEWPORT_SIZE       0x6d74u     /* width 29:16, height 13:0 */
#define AMDGPU_CRTC_CONTROL        0x6e70u     /* MASTER_EN 0 */
#define AMDGPU_CRTC_STATUS         0x6e8cu     /* V_BLANK 0 */
#define AMDGPU_CRTC_FRAME_COUNT    0x6e98u

/*----------------------------------------------------------------------
 * The chip.
 *--------------------------------------------------------------------*/

enum amdgpu_family {
    AMDGPU_NOT_SI = 0,          /* AMD, and not a Southern Islands chip this knows */
    AMDGPU_TAHITI,
    AMDGPU_PITCAIRN,
    AMDGPU_VERDE,
    AMDGPU_OLAND,
    AMDGPU_HAINAN,
};

struct amdgpu_family_info {
    const char *name;           /* "Tahiti" */
    unsigned    shader_engines; /* `si_gpu_init`'s max_shader_engines */
    unsigned    sh_per_se;      /*              max_sh_per_se */
    unsigned    cu_per_sh;      /*              max_cu_per_sh */
    unsigned    crtcs;          /* `radeon_asic.c`'s num_crtc */
};

/* Which Southern Islands chip a PCI device id is (`drm_pciids.h`). */
enum amdgpu_family amdgpu_family_of(uint16_t device);
const struct amdgpu_family_info *amdgpu_family_info(enum amdgpu_family family);

/*
 * The card's name, from the device id and the subsystem word - PCI
 * configuration offset 0x2c, the subsystem vendor in 15:0 and the
 * subsystem id in 31:16 - by the PCI ID Repository (`pci.ids`): the Mac
 * Pro's three are Apple's subsystems of 6810, 679e and 6798. NULL for a
 * card the table does not name.
 */
const char *amdgpu_card_name(uint16_t device, uint32_t subsystem);

/*----------------------------------------------------------------------
 * Its memory.
 *--------------------------------------------------------------------*/

/* VRAM in MB from CONFIG_MEMSIZE, garbage in 31:16 handled as `si_mc_init`. */
uint32_t amdgpu_vram_mb(uint32_t config_memsize);

/* The memory's width in bits from MC_ARB_RAMCFG and MC_SHARED_CHMAP. */
unsigned amdgpu_vram_bits(uint32_t ramcfg, uint32_t chmap);

/* Where VRAM sits in the GPU's own addresses: [start, end], end inclusive. */
void amdgpu_fb_location(uint32_t fb_location, uint64_t *start, uint64_t *end);

/*
 * An MC address as an offset into BAR 0, which begins at the MC address
 * HDP_NONSURFACE_BASE says (`si_mc_program` writes vram_start >> 8 there):
 * false when the address is not in the `aperture` bytes the BAR shows.
 */
bool amdgpu_mc_to_bar(uint64_t mc, uint32_t hdp_nonsurface_base, uint64_t aperture,
                      uint64_t *offset);

/* The active compute units of one shader array, as `si_get_cu_active_bitmap`. */
unsigned amdgpu_cus_active(uint32_t cc_shader_array, uint32_t user_shader_array,
                           unsigned cu_per_sh);

/* The die's temperature in degrees, as `si_get_temp` reads CTF_TEMP. */
unsigned amdgpu_temperature(uint32_t cg_mult_thermal_status);

/*----------------------------------------------------------------------
 * A display controller.
 *--------------------------------------------------------------------*/

uint32_t amdgpu_crtc_offset(unsigned crtc);

struct amdgpu_grph_regs {
    uint32_t crtc_control, grph_enable, grph_control, primary, primary_high;
    uint32_t pitch, x_start, y_start, x_end, y_end, viewport_size, update;
};

struct amdgpu_grph {
    bool        crtc_on;        /* CRTC_CONTROL.MASTER_EN */
    bool        enabled;        /* GRPH_ENABLE */
    unsigned    bpp;            /* 8, 16, 32 or 64; 0 for none known */
    unsigned    format;         /* GRPH_FORMAT, meaning by depth */
    unsigned    array_mode;     /* 0 linear general, 1 linear aligned, 2 1D, 4 2D tiled */
    bool        linear;
    uint64_t    address;        /* the primary surface, an MC address */
    unsigned    pitch_px;
    unsigned    width, height;  /* GRPH_X_END - X_START, Y the same */
    unsigned    view_w, view_h; /* the viewport */
    bool        pending;        /* a flip written and not yet taken */
};

void amdgpu_decode_grph(const struct amdgpu_grph_regs *r, struct amdgpu_grph *out);

/* "linear", "1D tiled", "2D tiled" or "mode n". */
const char *amdgpu_array_mode_name(unsigned mode);

/*----------------------------------------------------------------------
 * The DMA engines' commands (`sid.h` DMA_PACKET; `si_dma.c`).
 *
 * Each writes its dwords at `out` and returns how many. Addresses are MC
 * addresses, 40 bits. Copies are in bytes, at most AMDGPU_DMA_MAX_BYTES a
 * command; fills in dwords, at most the same. The ring takes them as the
 * CPU wrote them, little-endian, which is what both machines are.
 *--------------------------------------------------------------------*/

#define AMDGPU_DMA_MAX_BYTES       0xffff8u    /* amdgpu's copy_max_bytes and fill_max_bytes */
#define AMDGPU_DMA_WRITE           0x2u
#define AMDGPU_DMA_COPY            0x3u
#define AMDGPU_DMA_FENCE           0x6u
#define AMDGPU_DMA_TRAP            0x7u
#define AMDGPU_DMA_FILL            0xdu
#define AMDGPU_DMA_NOP             0xfu

uint32_t amdgpu_dma_header(unsigned cmd, unsigned b, unsigned t, unsigned s, uint32_t n);
unsigned amdgpu_dma_write(uint32_t *out, uint64_t addr, uint32_t value);
unsigned amdgpu_dma_fill(uint32_t *out, uint64_t addr, uint32_t value, uint32_t bytes);
unsigned amdgpu_dma_copy(uint32_t *out, uint64_t dst, uint64_t src, uint32_t bytes);
unsigned amdgpu_dma_fence(uint32_t *out, uint64_t addr, uint32_t seq);
unsigned amdgpu_dma_nop(uint32_t *out);

/* DMA_RB_CNTL's size field for a ring of `bytes`, a power of two: log2 dwords << 1. */
uint32_t amdgpu_dma_rb_size(uint32_t bytes);

/* DMA_RB_WPTR's value for a ring position in dwords (`si_dma_ring_set_wptr`). */
uint32_t amdgpu_dma_wptr(uint32_t dwords);

#endif /* KOSMOS_DRIVERS_DISPLAY_AMDGPU_DECODE_H */
