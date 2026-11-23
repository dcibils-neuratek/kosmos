/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_KITS_DISPLAY_EDID_H
#define KOSMOS_KITS_DISPLAY_EDID_H

/*
 * **The EDID kit** (`docs/displaymodes.md` D2): what a monitor says about
 * itself, decoded - its maker, product, serial and name, its size, and
 * every mode it lists, from the base block, a CTA-861 extension and a
 * DisplayID extension.
 *
 * A pure decoder over a byte array: no allocation, no system call, no
 * state between calls. `intelgpu` (the monitor read over AUX or GMBUS, D3)
 * and the window server link it; Lua sees only its answer, already
 * decoded, through `display.lua`. Held on the Mac by `tools/test_edid.c`.
 *
 * **The layout is Linux's, v6.12** - `include/drm/drm_edid.h` (the block's
 * fields, the descriptor tags, the range descriptor), `drivers/gpu/drm/
 * drm_edid.c` (how each kind of timing becomes a mode: `drm_mode_detailed`,
 * `drm_mode_std`, `add_established_modes`, `drm_est3_modes`, the CTA data
 * blocks, `drm_mode_displayid_detailed`; and the DMT, established and CTA
 * mode tables, which `edid.c` carries converted), `drm_displayid.c` and
 * `drm_displayid_internal.h` (DisplayID's sections and blocks), and
 * `drm_modes.c` (`drm_cvt_mode`, `drm_gtf_mode`). Where this decoder
 * departs from DRM it says so beside the code.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDID_BLOCK          128u
#define EDID_MODES_MAX      64u     /* a 48-mode list is the document's; room to spare */

/* Where a mode came from, in the order they are taken - the first of two
 * modes the same size at the same refresh, to the hertz, is the one kept. */
enum edid_source {
    EDID_FROM_DETAILED = 1,         /* the base block's detailed timings */
    EDID_FROM_DISPLAYID,            /* a DisplayID Type I or Type VII timing */
    EDID_FROM_CTA_DETAILED,         /* a CTA-861 extension's detailed timings */
    EDID_FROM_CTA_VIC,              /* a CTA video data block's code */
    EDID_FROM_STANDARD,             /* a standard timing, from DMT or a formula */
    EDID_FROM_ESTABLISHED_III,      /* the established timings III descriptor */
    EDID_FROM_ESTABLISHED,          /* the three bytes of established timings */
};

#define EDID_MODE_PREFERRED     (1u << 0)
#define EDID_MODE_INTERLACED    (1u << 1)   /* refresh_mhz is fields, as DRM's */
#define EDID_MODE_NATIVE        (1u << 2)   /* a CTA code marked native */
#define EDID_MODE_HSYNC_POS     (1u << 3)
#define EDID_MODE_VSYNC_POS     (1u << 4)
#define EDID_MODE_COMPUTED      (1u << 5)   /* a standard timing by CVT or GTF, not a table */

struct edid_mode {
    uint16_t width, height;
    uint16_t hsync_start, hsync_end, htotal;
    uint16_t vsync_start, vsync_end, vtotal;
    uint32_t clock_khz;
    uint32_t refresh_mhz;
    uint8_t  source;                /* enum edid_source */
    uint8_t  flags;
    uint8_t  vic;                   /* the CTA code, for EDID_FROM_CTA_VIC */
};

/* The range limits descriptor (0xFD). */
struct edid_range {
    bool     present;
    unsigned kind;                  /* byte 10: 0 GTF, 1 limits only, 2 secondary GTF, 4 CVT */
    unsigned min_vhz, max_vhz;
    unsigned min_hkhz, max_hkhz;
    unsigned max_clock_khz;         /* 0 when not given */
    bool     reduced_blanking;      /* CVT's: the monitor takes reduced blanking */
};

/* Problems that left a mode or a block out, without refusing the rest. */
#define EDID_PROBLEM_EXT_CHECKSUM   (1u << 0)   /* an extension's sum is not zero */
#define EDID_PROBLEM_EXT_MISSING    (1u << 1)   /* the count says more blocks than the bytes hold */
#define EDID_PROBLEM_EXT_UNKNOWN    (1u << 2)   /* an extension this kit does not decode */
#define EDID_PROBLEM_TOO_MANY_MODES (1u << 3)
#define EDID_PROBLEM_DISPLAYID      (1u << 4)   /* a DisplayID section cut short or its sum wrong */
#define EDID_PROBLEM_CTA            (1u << 5)   /* a CTA data block running past its block */
#define EDID_PROBLEM_UNKNOWN_VIC    (1u << 6)   /* a video code not in CTA-861's table */
#define EDID_PROBLEM_BAD_DETAILED   (1u << 7)   /* a detailed timing DRM would refuse */

/* Why a whole EDID was refused. */
enum edid_status {
    EDID_OK = 0,
    EDID_ERR_SHORT,                 /* fewer than 128 bytes */
    EDID_ERR_HEADER,                /* not 00 FF FF FF FF FF FF 00 */
    EDID_ERR_CHECKSUM,              /* the base block's sum is not zero */
    EDID_ERR_VERSION,               /* not EDID 1.x */
};

struct edid_info {
    char     maker[4];              /* the PNP id, three letters */
    uint16_t product;
    uint32_t serial;
    char     name[14];              /* the name descriptor (0xFC), or "" */
    char     serial_text[14];       /* the serial descriptor (0xFF), or "" */
    unsigned week, year;            /* of manufacture; year 0 when not given */
    unsigned version, revision;     /* 1.3, 1.4 */
    bool     digital;
    unsigned bpc;                   /* 1.4's colour depth, 0 when not said */
    unsigned interface;             /* 1.4's: 1 DVI, 2 HDMI-a, 3 HDMI-b, 4 MDDI, 5 DP; 0 not said */
    unsigned width_mm, height_mm;   /* the preferred timing's, else the block's centimetres */
    struct edid_range range;

    unsigned extensions;            /* the count byte 126 claims */
    unsigned cta_blocks, displayid_blocks;
    bool     hdmi;                  /* an HDMI vendor block (OUI 00-0C-03) */
    unsigned hdmi_max_tmds_khz;     /* from it, 0 when not given */
    bool     hdmi_forum;            /* an HDMI Forum block (OUI C4-5D-D8): HDMI 2 */
    unsigned hdmi_forum_max_tmds_khz;

    unsigned problems;              /* EDID_PROBLEM_* */
    unsigned nmodes;
    struct edid_mode modes[EDID_MODES_MAX];
};

/* Decodes `len` bytes - the base block and as many extensions as it says
 * and the bytes hold. EDID_OK, or why not; on a refusal `out` says
 * nothing. */
int edid_decode(const uint8_t *bytes, size_t len, struct edid_info *out);

const char *edid_status_name(int status);
const char *edid_source_name(unsigned source);

/* The diagonal, in tenths of an inch, from the millimetres; 0 without them. */
unsigned edid_diagonal_decinches(const struct edid_info *info);

/* The preferred mode: the first marked so, else the first; NULL with none. */
const struct edid_mode *edid_preferred(const struct edid_info *info);

#endif
