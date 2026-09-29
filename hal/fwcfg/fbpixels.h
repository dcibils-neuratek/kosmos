/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The pixels of a screen whose size the guest chooses - ramfb's and
 * virtio-gpu's, QEMU's two - at the size `opt/kosmos/fb` asks or the image
 * was built for (`roadmap.md` 6zt, 4h a). Both devices scan out of memory the
 * guest hands them; neither has an opinion about how large it is. So the
 * choice and the allocation are one copy, which ramfb held alone until there
 * was a second device to hold them for.
 */
#ifndef KOSMOS_HAL_FWCFG_FBPIXELS_H
#define KOSMOS_HAL_FWCFG_FBPIXELS_H

#include <stdbool.h>
#include <stdint.h>

/* Where the size came from: built, asked for, or asked for and refused. */
enum fb_size { FB_SIZE_BUILT, FB_SIZE_ASKED, FB_SIZE_REFUSED };

struct fb_pixels {
    uint8_t *pixels;            /* page-aligned, from the page allocator */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;             /* bytes per row; never width * 4 here */
    enum fb_size from;
};

/*
 * The pixels, zeroed, taken once: a second call - a device that answered
 * after another did not - is given the same memory rather than a second
 * screen's worth. False when the machine cannot spare them contiguous.
 */
bool fb_pixels_take(struct fb_pixels *out);

/* Where the size came from, for a board's `hal_fb_describe`. */
enum fb_size fb_pixels_from(void);

#endif
