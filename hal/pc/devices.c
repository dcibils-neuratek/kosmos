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
 *
 * **And the Intel display engine's backlight**, for a driver that reads it -
 * and, next, sets it. Device 0/2/0 is Intel's integrated graphics by the
 * chip's own convention: the Tiger Lake PRM names its registers by that
 * address (Vol 2c, `GTTMMADR_0_2_0_PCI`), and its vendor, its class and its
 * BAR's type are all checked before anything is believed: q35 puts an Intel
 * network card at that slot, 8086:10d3, and two of the three tell them apart
 * - class 02h rather than 03h, and a 32-bit BAR where GTTMMADR is 64. So
 * either alone would do, and a control has to take away both. GTTMMADR is
 * BAR0, 64 bits at configuration offset 10h with memory type 10b in bits
 * 2:1, 16 MB of which the first 2 MB are the MMIO registers; the base is
 * bits 63:24.
 *
 * The block's page, C8000h, holds the south display's two PWM controllers.
 * **That offset is Linux's, not Intel's**: no public manual for Tiger Lake
 * or Ice Lake documents the south display's backlight - they document the
 * utility pin's, which a laptop panel does not use (Vol 12: `L_BKLTCTL`,
 * "South display backlight PWM output") - and `intel_backlight_regs.h` puts
 * `_BXT_BLC_PWM_CTL1` at C8250h and uses it from Cannon Point on. So the
 * driver reads before anything writes, and the ThinkPad's reading is what
 * confirms it.
 *
 * **Neither sized nor enabled.** This device is scanning out the screen, its
 * memory decoding is on because the firmware turned it on, and turning that
 * off to size a BAR whose size the manual gives would be a risk for nothing.
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

#define CLASS_DISPLAY       0x03u
#define INTEL               0x8086u
#define IGD_SLOT            2u
#define IGD_BAR0_LOW        0x10u
#define IGD_BAR0_HIGH       0x14u
#define IGD_BAR0_64BIT      0x4u        /* bits 2:1 = 10b */
#define IGD_BASE_MASK       0xFF000000u /* GTTMMADR 63:24, low half */
#define BACKLIGHT_BLOCK     0xC8000u
#define BACKLIGHT_BYTES     0x1000u

/*
 * **Intel Ethernet**, class 2 subclass 0 with Intel's vendor identifier -
 * which is the I219 on Diego's ThinkCentre M700 (`roadmap.md` 5zd-f) and the
 * 82540EM and 82574L QEMU offers, all of one register family.
 *
 * By class *and* vendor rather than by device id, because the id differs
 * with every chipset and the register set does not; and by vendor as well as
 * class, because virtio-net is class 2 subclass 0 too and is the kernel's
 * own.
 */
#define CLASS_NETWORK       0x02u
#define SUBCLASS_ETHERNET   0x00u
#define ETHERNET_KEPT       2u

static struct spinlock devices_lock = SPINLOCK("devices");
static struct hal_device xhci_kept[XHCI_KEPT];
static bool xhci_known[XHCI_KEPT];
static struct hal_device ether_kept[ETHERNET_KEPT];
static bool ether_known[ETHERNET_KEPT];

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

static bool nth_intel_ethernet(unsigned index, struct pci_device *out)
{
    unsigned from = 0, at, seen = 0;

    while (pci_find_class(CLASS_NETWORK, SUBCLASS_ETHERNET, from, out, &at)) {
        if (out->vendor == INTEL) {
            if (seen == index) {
                return true;
            }

            seen++;
        }

        from = at + 1;
    }

    return false;
}

/*
 * Asked afresh each time, and nothing kept: three configuration reads, and
 * nothing is enabled or sized that a second answer could spend twice.
 */
static bool intel_backlight(struct hal_device *out)
{
    uint32_t id = pci_config_read(0, IGD_SLOT, 0, 0x00);
    uint32_t class_word = pci_config_read(0, IGD_SLOT, 0, PCI_CLASS_WORD);
    uint32_t low = pci_config_read(0, IGD_SLOT, 0, IGD_BAR0_LOW);
    uint64_t base;

    if ((id & 0xFFFFu) != INTEL || (class_word >> 24) != CLASS_DISPLAY
        || (low & 0x7u) != IGD_BAR0_64BIT) {
        return false;
    }

    base = ((uint64_t)pci_config_read(0, IGD_SLOT, 0, IGD_BAR0_HIGH) << 32)
         | (low & IGD_BASE_MASK);

    if (base == 0) {
        return false;
    }

    out->base  = (unsigned long)(base + BACKLIGHT_BLOCK);
    out->size  = BACKLIGHT_BYTES;
    out->intid = 0;
    out->line  = 0;
    out->where = IGD_SLOT << 3;

    return true;
}

bool hal_device_find(unsigned kind, unsigned index, struct hal_device *out)
{
    unsigned long flags;
    bool found = false;

    if (out != NULL && kind == HAL_DEV_INTEL_BACKLIGHT) {
        return index == 0 && intel_backlight(out);
    }

    /*
     * **Enabled and sized once, and kept**, for the reason the controllers
     * above are: `pci_enable` switches the device to MSI and `pci_bar_size`
     * writes all ones into a BAR with decoding off, and neither is a thing
     * to do twice to a device a driver is already using.
     */
    if (out != NULL && kind == HAL_DEV_INTEL_ETHERNET
        && index < ETHERNET_KEPT) {
        bool found = false;

        flags = spin_lock(&devices_lock);

        if (!ether_known[index]) {
            struct pci_device pci;
            uint64_t size;

            if (nth_intel_ethernet(index, &pci) && pci.bar[0] != 0
                && (size = pci_bar_size(&pci, 0)) != 0) {
                pci_enable(&pci);

                ether_kept[index].base  = (unsigned long)pci.bar[0];
                ether_kept[index].size  = (unsigned long)size;
                ether_kept[index].intid = pci.irq;
                ether_kept[index].line  = pci.device;
                ether_kept[index].where = ((unsigned)pci.bus << 8)
                                        | ((unsigned)pci.slot << 3)
                                        | (unsigned)pci.function;
                ether_known[index] = true;
            }
        }

        if (ether_known[index]) {
            *out = ether_kept[index];
            found = true;
        }

        spin_unlock(&devices_lock, flags);
        return found;
    }

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
