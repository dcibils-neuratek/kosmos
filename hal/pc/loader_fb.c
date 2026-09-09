/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Whether the loader really handed over a framebuffer this system can draw
 * into - and nothing else, on purpose.
 *
 * **A pure function, so it can be checked without a machine.** The path it
 * guards is the one thing about the first hardware boot that cannot be
 * tested under QEMU: `-kernel` ignores multiboot's video request, so the
 * flag is never set and the fallback always wins. On a laptop it is the
 * only way to get a screen at all, and a mistake in it is a black panel on
 * a machine with no serial port to complain over.
 *
 * So the decision is separated from the reading. `tools/test_loaderfb.c`
 * feeds it structures a loader might plausibly hand over, including the
 * wrong ones, and `hal/pc/memory.c` calls it for real - which is the same
 * split `kfs.lua` uses to check a filesystem format on a Mac.
 *
 * **Every rejection is a thing that has happened to somebody.** A loader
 * that sets the flag and fills in nothing; one that answers with a palette
 * or with EGA text, neither of which anything above `struct fb` can draw
 * into; one that gives 24 bits per pixel, where every write above here
 * would be the wrong width because this system treats a pixel as one
 * 32-bit word.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "multiboot.h"

bool pc_framebuffer_from(const struct multiboot_info *info,
                         struct pc_loader_fb *out)
{
    if (info == NULL || (info->flags & MB_FLAG_FRAMEBUFFER) == 0) {
        return false;
    }

    if (info->framebuffer_type != MB_FB_RGB || info->framebuffer_bpp != 32) {
        return false;
    }

    if (info->framebuffer_addr == 0 || info->framebuffer_width == 0
        || info->framebuffer_height == 0 || info->framebuffer_pitch == 0) {
        return false;
    }

    /*
     * A pitch narrower than the row it describes is a structure that
     * disagrees with itself, and believing it would walk off the end of
     * every row. `gfx.md` says the pitch is almost never width * 4; it is
     * never *less* either.
     */
    if (info->framebuffer_pitch < info->framebuffer_width * 4u) {
        return false;
    }

    out->addr = info->framebuffer_addr;
    out->pitch = info->framebuffer_pitch;
    out->width = info->framebuffer_width;
    out->height = info->framebuffer_height;

    return true;
}
