/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The devices on this board that a driver outside the kernel may be told
 * about. `hal/hal.h` has the argument for why the answer lives here.
 *
 * One today, and deliberately a function rather than a table: a table would
 * be a shape chosen before there was a second entry to say what the shape
 * should be.
 */

#include "hal.h"
#include "qemu-virt.h"

bool hal_device_find(unsigned kind, unsigned index, struct hal_device *out)
{
    if (out == NULL) {
        return false;
    }

    if (kind == HAL_DEV_PL061_POWER_KEY && index == 0) {
        out->base  = PL061_BASE;
        out->size  = PL061_SIZE;
        out->intid = PL061_INTID;
        out->line  = PL061_POWER_KEY_LINE;
        out->where = 0;
        return true;
    }

    return false;
}
