/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The devices on a PC that a driver outside the kernel may be told about.
 *
 * **xHCI controllers**, found on PCI by what they are rather than who made
 * them: class 0Ch, a serial bus; subclass 03h, USB; programming interface
 * 30h, xHCI. The same class and subclass with interface 00h, 10h and 20h are
 * UHCI, OHCI and EHCI, which is why the interface is compared as well. QEMU's
 * `qemu-xhci` reads as 1b36:000d, class 0c0330, in this system's own bus
 * listing, which is where the numbers were checked.
 *
 * `index` counts xHCI controllers alone, in bus order. A laptop of the
 * ThinkPad's generation is expected to carry two - one in the chipset and one
 * for its USB-C ports - and that is the reason there is an index at all.
 *
 * **Enabled once, and the answer kept.** `pci_enable` switches a device to
 * MSI and takes a vector from a range of four every time it is called, so a
 * driver that asked twice would spend two. The BAR is sized before it, with
 * decoding off, and the kept answer is what every later question gets.
 *
 * A PC's power button is an ACPI event rather than a GPIO line, so that kind
 * is still none here.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal.h"
#include "pci.h"
#include "spinlock.h"

#define CLASS_SERIAL_BUS    0x0Cu
#define SUBCLASS_USB        0x03u
#define INTERFACE_XHCI      0x30u
#define PCI_CLASS_WORD      0x08u   /* revision, interface, subclass, class */

/* As many controllers as are kept; a machine with more is told about four. */
#define XHCI_KEPT           4u

static struct spinlock devices_lock = SPINLOCK("devices");
static struct hal_device xhci_kept[XHCI_KEPT];
static bool xhci_known[XHCI_KEPT];

static bool nth_xhci(unsigned index, struct pci_device *out)
{
    unsigned from = 0, at, seen = 0;

    while (pci_find_class(CLASS_SERIAL_BUS, SUBCLASS_USB, from, out, &at)) {
        uint32_t word = pci_config_read(out->bus, out->slot, out->function,
                                        PCI_CLASS_WORD);

        if (((word >> 8) & 0xFFu) == INTERFACE_XHCI) {
            if (seen == index) {
                return true;
            }

            seen++;
        }

        from = at + 1;
    }

    return false;
}

bool hal_device_find(unsigned kind, unsigned index, struct hal_device *out)
{
    unsigned long flags;
    bool found = false;

    if (out == NULL || kind != HAL_DEV_XHCI || index >= XHCI_KEPT) {
        return false;
    }

    flags = spin_lock(&devices_lock);

    if (!xhci_known[index]) {
        struct pci_device pci;
        uint64_t size;

        if (nth_xhci(index, &pci) && pci.bar[0] != 0
            && (size = pci_bar_size(&pci, 0)) != 0) {
            pci_enable(&pci);

            xhci_kept[index].base  = (unsigned long)pci.bar[0];
            xhci_kept[index].size  = (unsigned long)size;
            xhci_kept[index].intid = pci.irq;
            xhci_kept[index].line  = 0;
            xhci_kept[index].where = ((unsigned)pci.bus << 8)
                                   | ((unsigned)pci.slot << 3)
                                   | (unsigned)pci.function;
            xhci_known[index] = true;
        }
    }

    if (xhci_known[index]) {
        *out = xhci_kept[index];
        found = true;
    }

    spin_unlock(&devices_lock, flags);

    return found;
}
