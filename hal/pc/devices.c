/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The devices on a PC that a driver outside the kernel may be told about.
 *
 * None yet, and saying so is the implementation. A PC's power button is an
 * ACPI event rather than a GPIO line, so the one kind a driver asks for today
 * does not exist here; the first real entry on this board will be the xHCI
 * controller, found through PCI rather than written down, and it will arrive
 * with the driver that needs it.
 */

#include "hal.h"

bool hal_device_find(unsigned kind, struct hal_device *out)
{
    (void)kind;
    (void)out;

    return false;
}
