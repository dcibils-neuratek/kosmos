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

/*
 * What this machine is called: nothing is read, and that is said.
 *
 * This board boots with no firmware in front of it and looks for no SMBIOS.
 * The build's platform string is true here by construction - every address
 * in this directory is `virt`'s and the board runs nowhere else - so that is
 * what a caller shows instead, and `source` says why it has to.
 */
bool hal_machine_ident(struct hal_machine *out)
{
    static const struct hal_machine none = {
        .source = "this board does not read SMBIOS",
    };

    if (out == NULL) {
        return false;
    }

    *out = none;
    return false;
}

/*
 * No firmware tables: `virt` is described by a device tree rather than by
 * ACPI, and this board reads neither - every address here is compiled in.
 */
unsigned hal_firmware_init(void)
{
    return 0;
}

bool hal_firmware_table(unsigned index, struct hal_firmware_table *out)
{
    (void)index;
    (void)out;
    return false;
}
