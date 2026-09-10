/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The one part of the first hardware boot that QEMU cannot exercise.
 *
 * A real PC gets its screen from whatever the loader set up: multiboot's
 * video request asks, and bit 12 of the info flags says it was answered.
 * **QEMU's `-kernel` never answers it** - measured, not assumed - so the
 * flag is never set under emulation and `hal/pc/fb.c` always takes the
 * ramfb fallback. On a ThinkPad the fallback does not exist and this is the
 * only path there is.
 *
 * So the decision is a pure function and this checks it on the host, the
 * same way `kfs.lua` checks a filesystem format on a Mac. What is left for
 * the machine is whether GRUB fills the fields in - which is one boot to
 * find out, rather than a driver to debug on a panel that stays black.
 *
 * Every rejection below is something a loader has really done to somebody.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hal/pc/multiboot.h"
#include "../hal/pc/multiboot2.h"

static int checks;
static int failures;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

/* A structure a working loader would hand over. */
static void good(struct multiboot_info *info)
{
    memset(info, 0, sizeof(*info));

    info->flags = MB_FLAG_FRAMEBUFFER;
    info->framebuffer_addr = 0xe0000000ull;
    info->framebuffer_pitch = 7744;      /* not width * 4, deliberately */
    info->framebuffer_width = 1920;
    info->framebuffer_height = 1080;
    info->framebuffer_bpp = 32;
    info->framebuffer_type = MB_FB_RGB;
}

int main(void)
{
    struct multiboot_info info;
    struct pc_loader_fb fb;

    /* The ordinary case, and the fields come back as they went in. */
    good(&info);
    check(pc_framebuffer_from(&info, &fb), "a good framebuffer was refused");
    check(fb.addr == 0xe0000000ull, "the address did not come back");
    check(fb.width == 1920 && fb.height == 1080, "the size did not come back");

    /*
     * **The pitch is the field this exists for.** `gfx.md` says it is
     * almost never width * 4, and a driver that assumed it was would shear
     * every row on a panel whose stride is padded - which is exactly what
     * 7744 against 1920 is.
     */
    check(fb.pitch == 7744, "the pitch was replaced with something tidier");

    /* No flag: the loader ignored the request, which QEMU does every time. */
    good(&info);
    info.flags = 0;
    check(!pc_framebuffer_from(&info, &fb),
          "a framebuffer was taken from a loader that never offered one");

    /* The flag set and nothing behind it. */
    good(&info);
    info.framebuffer_addr = 0;
    check(!pc_framebuffer_from(&info, &fb), "a null framebuffer was accepted");

    /* A palette, and EGA text. Neither is something to draw into. */
    good(&info);
    info.framebuffer_type = 0;
    check(!pc_framebuffer_from(&info, &fb), "an indexed framebuffer was accepted");

    good(&info);
    info.framebuffer_type = 2;
    check(!pc_framebuffer_from(&info, &fb), "an EGA text mode was accepted");

    /* 24 bits per pixel: every write above here would be the wrong width. */
    good(&info);
    info.framebuffer_bpp = 24;
    check(!pc_framebuffer_from(&info, &fb), "24bpp was accepted");

    good(&info);
    info.framebuffer_bpp = 16;
    check(!pc_framebuffer_from(&info, &fb), "16bpp was accepted");

    /* Zero dimensions, which a loader that failed halfway can produce. */
    good(&info);
    info.framebuffer_width = 0;
    check(!pc_framebuffer_from(&info, &fb), "a zero width was accepted");

    good(&info);
    info.framebuffer_height = 0;
    check(!pc_framebuffer_from(&info, &fb), "a zero height was accepted");

    /*
     * A pitch narrower than the row it describes. The structure disagrees
     * with itself, and believing it walks off the end of every row.
     */
    good(&info);
    info.framebuffer_pitch = 1920 * 4 - 4;
    check(!pc_framebuffer_from(&info, &fb),
          "a pitch narrower than the row was accepted");

    /* Exactly width * 4 is legal - it is only *less* that cannot be. */
    good(&info);
    info.framebuffer_pitch = 1920 * 4;
    check(pc_framebuffer_from(&info, &fb),
          "an unpadded pitch was refused");

    /*----------------------------------------------------------------
     * And the other protocol's structure, whose *walk* is the new part.
     *
     * Multiboot 1 is one flat structure with a flags word; Multiboot 2 is
     * a chain of tags, and the walk is where a malformed one can hurt. A
     * size below the header never advances - a hang at boot. A step that
     * is not rounded up to eight finds the next tag at the wrong offset,
     * which reads one tag correctly and then nonsense, the most confusing
     * shape a bug can have.
     *---------------------------------------------------------------*/
    {
        static unsigned char blob[128];
        struct mb2_info *mb2 = (struct mb2_info *)blob;
        struct mb2_tag_framebuffer *tfb;
        struct mb2_tag *end;
        struct pc_loader_fb out;

        memset(blob, 0, sizeof(blob));

        tfb = (struct mb2_tag_framebuffer *)(blob + sizeof(*mb2));
        tfb->tag.type = MB2_TAG_FRAMEBUFFER;
        tfb->tag.size = sizeof(*tfb);
        tfb->addr = 0x80000000u;
        tfb->pitch = 5120;
        tfb->width = 1280;
        tfb->height = 800;
        tfb->bpp = 32;
        tfb->fb_type = MB2_FB_RGB;

        end = (struct mb2_tag *)((unsigned char *)tfb
                                 + ((sizeof(*tfb) + 7u) & ~7u));
        end->type = MB2_TAG_END;
        end->size = 8;

        mb2->total_size = (uint32_t)((unsigned char *)end + 8 - blob);

        check(mb2_find(mb2, MB2_TAG_FRAMEBUFFER) == &tfb->tag,
              "the walk did not find a tag that is there");
        check(mb2_find(mb2, MB2_TAG_ACPI_NEW) == NULL,
              "the walk found a tag that is not there");
        check(mb2_framebuffer_from(mb2, &out),
              "a good Multiboot 2 framebuffer was refused");
        check(out.addr == 0x80000000u && out.pitch == 5120
              && out.width == 1280 && out.height == 800,
              "the Multiboot 2 framebuffer came back changed");

        tfb->bpp = 24;
        check(!mb2_framebuffer_from(mb2, &out),
              "24 bits per pixel was accepted");
        tfb->bpp = 32;

        tfb->pitch = 1000;
        check(!mb2_framebuffer_from(mb2, &out),
              "a pitch narrower than the row was accepted");
        tfb->pitch = 5120;

        tfb->tag.size = 4;
        check(mb2_find(mb2, MB2_TAG_ACPI_NEW) == NULL,
              "a tag smaller than its own header did not stop the walk");
        tfb->tag.size = sizeof(*tfb);

        mb2->total_size = 4;
        check(mb2_find(mb2, MB2_TAG_FRAMEBUFFER) == NULL,
              "a structure shorter than its own header was walked");

        check(mb2_find(NULL, MB2_TAG_FRAMEBUFFER) == NULL,
              "a null structure was walked");
    }

    if (failures > 0) {
        printf("FAIL: %d of %d checks on the loader's framebuffer.\n",
               failures, checks + failures);
        return 1;
    }

    printf("PASS: %d checks on the framebuffer a loader hands over, on this "
           "machine (the one path QEMU cannot exercise).\n", checks);
    return 0;
}
