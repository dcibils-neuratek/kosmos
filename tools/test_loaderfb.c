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

    if (failures > 0) {
        printf("FAIL: %d of %d checks on the loader's framebuffer.\n",
               failures, checks + failures);
        return 1;
    }

    printf("PASS: %d checks on the framebuffer a loader hands over, on this "
           "machine (the one path QEMU cannot exercise).\n", checks);
    return 0;
}
