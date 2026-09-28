/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where this board's pixels come from: ramfb, and nothing else.
 *
 * A pass-through, and it exists for the same reason `input_bind.c` does -
 * the PC has two possible answers and has to choose between them, so the
 * HAL name belongs to a board rather than to a driver.
 *
 * The Pi will replace this file rather than edit one: its firmware answers
 * a mailbox with an address it chose, which is neither of the two things
 * the other boards do.
 */

#include <stdbool.h>

#include "hal.h"
#include "ramfb.h"

bool hal_fb_init(struct fb *out)
{
    return ramfb_init(out);
}

/* With where its size came from (`opt/kosmos/fb`, `roadmap.md` 6zt). */
const char *hal_fb_describe(void)
{
    switch (ramfb_size_from()) {
    case RAMFB_SIZE_ASKED:
        return "ramfb, the way the Pi's mailbox will be, at the size "
               "opt/kosmos/fb asked";
    case RAMFB_SIZE_REFUSED:
        return "ramfb, the way the Pi's mailbox will be, at the size it was "
               "built for: opt/kosmos/fb asked for none it can show";
    default:
        return "ramfb, the way the Pi's mailbox will be, at the size it was "
               "built for";
    }
}

/*
 * Not on this board, and nothing is lost by it.
 *
 * ramfb is the guest allocating pixels and telling the hypervisor to scan
 * them out, and "the guest allocating" is exactly what does not exist
 * before `pmm_init`. The board this matters on is the one with no serial
 * port; `virt` has one, and every boot here already reads its whole log
 * over a cable.
 */
bool hal_fb_early(struct fb *out)
{
    (void)out;

    return false;
}

/* Nothing said yes to `hal_fb_early` here, so nothing can need remapping. */
bool hal_fb_remap(struct fb *out)
{
    (void)out;

    return false;
}
