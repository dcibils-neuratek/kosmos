/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The display, on both of QEMU's machines: ramfb.
 *
 * **Shared rather than copied, because there is nothing board-specific in
 * it.** ramfb is a fw_cfg *file* - the guest allocates the pixels and tells
 * the firmware where they are, and from then on QEMU scans them out - so
 * the whole of the driver is one write to an item found by name, and both
 * boards have fw_cfg. What differs is only how that item is reached, which
 * is the two functions in `fwcfg.h` and nothing here.
 *
 * ramfb is the simplest framebuffer QEMU has. There is no device to
 * enumerate, no queue to set up and no command to flush: the guest hands
 * QEMU a pointer, a format and a geometry through fw_cfg, and QEMU scans
 * that memory out from then on. Roughly a hundred lines against the eight
 * hundred that PCI enumeration plus virtqueues plus the virtio-gpu command
 * set would cost before a single pixel appeared.
 *
 * `roadmap.md` M6 says virtio-gpu, and it will get one - as a *second*
 * implementation of this same interface, which is when the flush half of it
 * earns its shape. Two reasons for the order:
 *
 *   - ramfb is what the HAL interface actually looks like. `hal_fb_init` is
 *     "ask the firmware for a linear framebuffer", and that is precisely the
 *     Pi's mailbox as well. virtio-gpu is the odd one out: it needs an
 *     explicit RESOURCE_FLUSH after drawing, so it is the target that will
 *     add `hal_fb_flush` - and `hal.md` is right that an interface invented
 *     against one target is that target's shape wearing generic names.
 *   - Everything above this file is identical either way. The backbuffer,
 *     the blitter, the font, the app server, the UI kit: none of it changes
 *     when the scanout does.
 *
 * What ramfb costs: no dirty rectangles. QEMU rescans the whole buffer on
 * its own schedule, so damage tracking in the compositor still saves the
 * drawing but cannot save the scanout, and there is no vblank to
 * synchronise with. Under emulation neither is the bottleneck.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hal.h"
#include "mmu.h"
#include "fwcfg.h"
#include "page.h"
#include "pmm.h"
#include "ramfb.h"
#include "fbpixels.h"

/*
 * The size and the pixels are `fbpixels.c`'s, shared with virtio-gpu: both
 * devices scan out of memory the guest chose, at a size the guest chose.
 */

/*
 * `struct RAMFBCfg` from QEMU's hw/display/ramfb.c, which is where it is
 * defined and the only place it is written down. Every field big-endian;
 * QEMU reads them with be32_to_cpu and be64_to_cpu. Packed, as upstream is:
 * a 64-bit field followed by five 32-bit ones is 28 bytes, and the compiler
 * would otherwise be free to make it 32.
 */
struct ramfb_cfg {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} __attribute__((packed));

_Static_assert(sizeof(struct ramfb_cfg) == 28, "RAMFBCfg is 28 bytes");

/*
 * DRM_FORMAT_XRGB8888, from include/standard-headers/drm/drm_fourcc.h:
 *
 *     #define fourcc_code(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) |
 *                                      ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
 *     #define DRM_FORMAT_XRGB8888 fourcc_code('X', 'R', '2', '4')
 *         /_ [31:0] x:R:G:B 8:8:8:8 little endian _/
 *
 * Written as the characters rather than as 0x34325258, because the number
 * says nothing and the characters are checkable against the header. QEMU
 * accepts it: qemu_drm_format_to_pixman maps it to PIXMAN_LE_x8r8g8b8.
 *
 * "little endian" in that comment describes the 32-bit word, so a uint32_t
 * pixel is 0x00RRGGBB and that is what `struct fb` promises.
 */
#define FOURCC(a, b, c, d)  ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                             ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define DRM_FORMAT_XRGB8888 FOURCC('X', 'R', '2', '4')

bool ramfb_init(struct fb *out)
{
    struct ramfb_cfg cfg;
    struct fb_pixels px;
    uint16_t select;
    uint32_t size;

    if (!fwcfg_present()) {
        return false;
    }

    /*
     * "etc/ramfb" is the name QEMU registers the item under. Absent when
     * the machine was started without `-device ramfb`, which is not a
     * failure: a serial-only boot is a legitimate way to run this system and
     * `make test` uses one.
     */
    if (!fwcfg_find("etc/ramfb", &select, &size)) {
        return false;
    }

    /* The item is the config structure and nothing else. A mismatch means
     * this kernel and this QEMU disagree about the layout, and writing 28
     * bytes into something that is not 28 bytes long is how a plausible
     * wrong image happens. */
    if (size != sizeof(cfg)) {
        return false;
    }

    /* The pixels, at the size asked or built (`fbpixels.c`). */
    if (!fb_pixels_take(&px)) {
        return false;
    }

    /* The address QEMU's device scans out of, so it is physical: the
     * hardware reads this memory without a page table, and on a board whose
     * kernel reaches RAM through a window the pointer and the address are
     * different numbers. */
    cfg.addr   = __builtin_bswap64((uint64_t)virt_to_phys(px.pixels));
    cfg.fourcc = __builtin_bswap32(DRM_FORMAT_XRGB8888);
    cfg.flags  = 0;
    cfg.width  = __builtin_bswap32(px.width);
    cfg.height = __builtin_bswap32(px.height);
    cfg.stride = __builtin_bswap32(px.pitch);

    /*
     * The write is what creates the display surface: QEMU's callback runs on
     * it and builds a pixman image over this memory. Everything drawn from
     * here on is scanned out without another word from us.
     */
    if (!fwcfg_write(select, &cfg, sizeof(cfg))) {
        return false;
    }

    out->pixels = (volatile uint32_t *)(void *)px.pixels;

    /* The guest chose this memory out of its own RAM and the kernel is
     * identity mapped, so the two are the same number here. */
    /* `phys` is what a process's mapping is built from, and `pixels` is
     * what this kernel writes through - the same memory, two names. */
    out->phys = virt_to_phys(px.pixels);
    out->width  = px.width;
    out->height = px.height;
    out->pitch  = px.pitch;

    return true;
}
