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

const char *hal_fb_describe(void)
{
    return "ramfb, the way the Pi's mailbox will be";
}
