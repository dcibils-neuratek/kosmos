/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The pixels of a screen whose size the guest chooses (`fbpixels.h`): the
 * size, and the memory. Moved here from `ramfb.c` whole, comments and all,
 * when virtio-gpu became the second device to want them (`roadmap.md` 4h a).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mmu.h"
#include "fwcfg.h"
#include "page.h"
#include "pmm.h"
#include "fbpixels.h"

/*
 * The geometry.
 *
 * **Chosen when the machine starts, not when the image is built**
 * (`roadmap.md` 6zt). Diego, 28 September 2026, running the release at
 * 3840x2160 and told there was no image of that size: "why is that images
 * need to be built for specific resolutions? cant that just be a parameter
 * and kosmos adapts to the resolution?" It could, and should have been
 * since the pixels came from the page allocator: this used to be fixed at
 * build time "because there is no allocator", the pixels a static array,
 * and when they moved nothing here moved with them.
 *
 * So `-fw_cfg name=opt/kosmos/fb,string=3840x2160`, read at the moment the
 * pixels are allocated, and the size the image was built with - `make
 * FB=1920x1080` - when there is none, or when it is no size this can show:
 * not two numbers, outside the bounds below, or more contiguous memory than
 * the machine has. With ramfb and virtio-gpu the guest chooses and QEMU makes a
 * window that size, so there is nobody else to ask.
 *
 * Nothing above this cares. The console works out its rows and columns from
 * what it is handed, `gfx.screen()` reports what the kernel reports, and the
 * window manager scales the pointer against the size it was told.
 *
 * Changing it while the machine runs is a different question and a real one;
 * see `hal.md`.
 */
#ifndef FB_WIDTH
#define FB_WIDTH    1024
#endif

#ifndef FB_HEIGHT
#define FB_HEIGHT   768
#endif

/*
 * **The stride is deliberately not width * 4.**
 *
 * These devices let the guest pick it, so this could be the tidy value, and that is
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
#define PITCH_OF(w) ((w) * 4u + 64u)

/*
 * The sizes it will take, and they are what ramfb and a screen are, not a
 * budget: a screen narrower than 640 or shorter than 480 is not one this
 * desktop draws, and 8K is past any display this runs on. Memory is the
 * other bound and it is asked rather than assumed.
 */
#define FB_MIN_W    640u
#define FB_MIN_H    480u
#define FB_MAX_W    7680u
#define FB_MAX_H    4320u

static uint32_t fb_width = FB_WIDTH, fb_height = FB_HEIGHT;
static enum fb_size size_from = FB_SIZE_BUILT;

/*
 * The pixels, **taken from the page allocator rather than reserved inside
 * the image** - and the move is worth the explanation, because the old
 * arrangement had reasons and they were all good ones.
 *
 * It was `_Alignas(4096) uint8_t framebuffer[FB_BYTES]` in section
 * `.framebuffer`, which the linker script places after the stacks and
 * inside `__image_end`: after the stacks so eight megabytes here would not
 * push the stack guards out of the page-mapped first 2 MB of RAM, inside
 * `__image_end` so the page allocator counts these pages as the kernel's
 * and never hands them out, and NOLOAD so the image file carries no
 * megabytes of zeroes.
 *
 * **What it cost was invisible until a real machine was booted from a USB
 * stick.** NOLOAD keeps the bytes out of the file and not out of the
 * *image*: `kosmos.bin` is 10.3 MB and `__image_end` was 19.7 MB, so a
 * loader is asked to find, reserve and zero nearly twice the kernel it
 * actually reads. On the ThinkPad, GRUB loading that alongside a 64 MB
 * module hands over an image with pages already corrupted - eleven of them,
 * contiguous, before this kernel has executed an instruction - and which
 * pages depends on the layout, so four kilobytes more kernel moves it
 * somewhere else or makes it vanish. `docs/thinkpad.md` §6a has the whole
 * account.
 *
 * Halving what the loader must place does not *fix* that. What it does is
 * stop this kernel asking for nine megabytes of address space it does not
 * use, which was never a thing worth asking for once there was an allocator
 * to ask instead.
 *
 * **And there is an allocator by the time this runs.** `hal_fb_init` is
 * boot stage six; `pmm_init` is stage four. The comment above this one said
 * "fixed at build time because there is no allocator", which stopped being
 * true when the display moved after physical memory in `kmain` and nobody
 * came back to it.
 *
 * `pmm_alloc_contiguous` answers with page-aligned memory, which is what
 * QEMU needs - it is handed the physical address - and what a mapping into
 * the app server's address space will need later. Pages the allocator has
 * given out are not handed out again, which is the other thing being inside
 * `__image_end` used to buy.
 */
static uint8_t *framebuffer;

/* "3840x2160" into two numbers, or false. Digits, an x, digits, and nothing
 * else: a value with anything more in it was not written for this. */
static bool parse_size(const char *s, uint32_t *w, uint32_t *h)
{
    uint32_t n[2] = { 0, 0 };
    unsigned which = 0, digits = 0;

    for (; *s != '\0'; s++) {
        if (*s >= '0' && *s <= '9') {
            if (n[which] > 100000u) {
                return false;
            }

            n[which] = n[which] * 10u + (uint32_t)(*s - '0');
            digits++;
        } else if ((*s == 'x' || *s == 'X') && which == 0 && digits > 0) {
            which = 1;
            digits = 0;
        } else {
            return false;
        }
    }

    if (which != 1 || digits == 0) {
        return false;
    }

    *w = n[0];
    *h = n[1];
    return true;
}

bool fb_pixels_take(struct fb_pixels *out)
{
    /*
     * Refused rather than half-drawn if the machine cannot spare them
     * contiguous: `hal_fb_init` treats false as "this board has no such
     * screen", falls through, and the boot log says which source answered -
     * which is the honest outcome, since a display that cannot be allocated
     * is a machine with no display.
     */
    if (framebuffer == NULL) {
        char asked[24];
        uint32_t w, h;

        /*
         * The size asked for, if it is one: tried first, and the built size
         * after it when the memory is not there - a screen at the size it
         * was built for is better than none at the size asked.
         */
        if (fwcfg_present()
            && fwcfg_boot_option("opt/kosmos/fb", asked, sizeof asked)) {
            size_from = FB_SIZE_REFUSED;

            if (parse_size(asked, &w, &h)
                && w >= FB_MIN_W && w <= FB_MAX_W
                && h >= FB_MIN_H && h <= FB_MAX_H) {
                framebuffer = pmm_alloc_contiguous(
                    (PITCH_OF(w) * h + PAGE_SIZE - 1) / PAGE_SIZE);

                if (framebuffer != NULL) {
                    fb_width = w;
                    fb_height = h;
                    size_from = FB_SIZE_ASKED;
                }
            }
        }

        if (framebuffer == NULL) {
            framebuffer = pmm_alloc_contiguous(
                (PITCH_OF(fb_width) * fb_height + PAGE_SIZE - 1) / PAGE_SIZE);
        }

        if (framebuffer == NULL) {
            return false;
        }
    }

    /* Zeroed here rather than by start.S, which only covered .bss and never
     * covered this at all. Black rather than whatever the last owner left,
     * and it is the first proof that these pages are writable. */
    memset(framebuffer, 0, PITCH_OF(fb_width) * fb_height);

    out->pixels = framebuffer;
    out->width  = fb_width;
    out->height = fb_height;
    out->pitch  = PITCH_OF(fb_width);
    out->from   = size_from;
    return true;
}

enum fb_size fb_pixels_from(void)
{
    return size_from;
}
