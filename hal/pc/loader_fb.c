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
#include "multiboot2.h"

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

/*------------------------------------------------------------------------
 * And the same questions of the other protocol's structure.
 *----------------------------------------------------------------------*/

const struct mb2_tag *mb2_find(const struct mb2_info *info, uint32_t type)
{
    const uint8_t *at;
    const uint8_t *end;

    if (info == NULL || info->total_size < sizeof(*info)) {
        return NULL;
    }

    at = (const uint8_t *)info + sizeof(*info);
    end = (const uint8_t *)info + info->total_size;

    while (at + sizeof(struct mb2_tag) <= end) {
        const struct mb2_tag *tag = (const struct mb2_tag *)at;

        if (tag->type == MB2_TAG_END) {
            return NULL;
        }

        /*
         * **A size smaller than the header is a walk that never
         * advances**, which is the one thing a malformed structure could do
         * to this function. The specification's own minimum is eight.
         */
        if (tag->size < sizeof(struct mb2_tag)) {
            return NULL;
        }

        if (tag->type == type) {
            return tag;
        }

        /*
         * Every tag begins on an eight-byte boundary whatever its size
         * says, so the step is the size rounded up rather than the size.
         * Reading the second tag at the wrong offset is how a walk finds
         * one tag and then nonsense.
         */
        at += (tag->size + 7u) & ~7u;
    }

    return NULL;
}

bool mb2_framebuffer_from(const struct mb2_info *info,
                          struct pc_loader_fb *out)
{
    const struct mb2_tag_framebuffer *fb =
        (const struct mb2_tag_framebuffer *)mb2_find(info,
                                                     MB2_TAG_FRAMEBUFFER);

    if (fb == NULL || fb->tag.size < sizeof(*fb)) {
        return false;
    }

    /* The same four refusals, for the same reasons as above: a palette or
     * EGA text is something nothing here can draw into, and a depth that
     * is not 32 makes every write above this the wrong width. */
    if (fb->fb_type != MB2_FB_RGB || fb->bpp != 32) {
        return false;
    }

    if (fb->addr == 0 || fb->width == 0 || fb->height == 0
        || fb->pitch == 0) {
        return false;
    }

    if (fb->pitch < fb->width * 4u) {
        return false;
    }

    out->addr = fb->addr;
    out->pitch = fb->pitch;
    out->width = fb->width;
    out->height = fb->height;

    return true;
}
