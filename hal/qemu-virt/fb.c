/*
 * The display, on QEMU virt: ramfb.
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
#include "qemu-virt.h"

/*
 * The geometry.
 *
 * Fixed at build time because there is no allocator: the pixels are a static
 * array, so their size has to be a constant. Asking ramfb what size the
 * window is would be asking the wrong question anyway - with ramfb the guest
 * chooses, and QEMU makes a window that size.
 */
#define FB_WIDTH    1024
#define FB_HEIGHT   768

/*
 * **The stride is deliberately not width * 4.**
 *
 * ramfb lets the guest pick it, so this could be the tidy value, and that is
 * exactly the reason not to. A framebuffer whose pitch happens to equal
 * width * 4 lets every address calculation in the system be written wrong
 * and still work perfectly, for months, until the first real board - where
 * the firmware picks a stride padded to whatever alignment it likes and
 * every one of those calculations produces a sheared image at once.
 *
 * `gfx.md` §19.3 puts this at the top of its list of traps and `CLAUDE.md`
 * makes it a rule: nothing in Lua computes a pixel offset, and all address
 * arithmetic happens inside the C primitives. Padding here is what turns a
 * violation of that rule into something visible on the first run instead of
 * at milestone 2.
 *
 * 64 bytes because it is a plausible alignment for real firmware to choose
 * and it is not a multiple of 4 pixels, so an off-by-one row is obvious
 * rather than subtle.
 */
#define FB_PITCH    (FB_WIDTH * 4 + 64)
#define FB_BYTES    (FB_PITCH * FB_HEIGHT)

/*
 * The pixels.
 *
 * In `.framebuffer`, which the linker script places after the stacks and
 * inside __image_end. After the stacks so that three megabytes here does not
 * push the stack guards out of the page-mapped first 2 MB of RAM; inside
 * __image_end so the page allocator counts these pages as the kernel's and
 * never hands them out. NOLOAD, so the image file does not carry three
 * megabytes of zeroes.
 *
 * Page aligned because QEMU is given the physical address and because the
 * app server will one day have this mapped into its own address space, and
 * a mapping is made of pages.
 */
static _Alignas(4096) uint8_t framebuffer[FB_BYTES]
    __attribute__((section(".framebuffer")));

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

bool hal_fb_init(struct fb *out)
{
    struct ramfb_cfg cfg;
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

    /* Zeroed here rather than by start.S, which only covers .bss. Black
     * rather than whatever the last boot left, and it is the first proof
     * that these three megabytes are writable. */
    memset(framebuffer, 0, sizeof(framebuffer));

    cfg.addr   = __builtin_bswap64((uint64_t)(uintptr_t)framebuffer);
    cfg.fourcc = __builtin_bswap32(DRM_FORMAT_XRGB8888);
    cfg.flags  = 0;
    cfg.width  = __builtin_bswap32(FB_WIDTH);
    cfg.height = __builtin_bswap32(FB_HEIGHT);
    cfg.stride = __builtin_bswap32(FB_PITCH);

    /*
     * The write is what creates the display surface: QEMU's callback runs on
     * it and builds a pixman image over this memory. Everything drawn from
     * here on is scanned out without another word from us.
     */
    if (!fwcfg_write(select, &cfg, sizeof(cfg))) {
        return false;
    }

    out->pixels = (volatile uint32_t *)framebuffer;
    out->width  = FB_WIDTH;
    out->height = FB_HEIGHT;
    out->pitch  = FB_PITCH;

    return true;
}
