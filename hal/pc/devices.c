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

/*
 * **A SATA controller in AHCI mode**: class 01 (mass storage), subclass 06
 * (SATA), interface 01 (AHCI 1.0) - PCI's class codes, as `lspci` and
 * Linux's `ahci` match them. Its registers are BAR5, the ABAR (AHCI 1.3.1
 * §2.1.11); the BARs below it are the legacy IDE windows, which this does
 * not use.
 */
#define CLASS_STORAGE       0x01u
#define SUBCLASS_SATA       0x06u
#define INTERFACE_AHCI      0x01u
#define AHCI_ABAR           5u
#define AHCI_KEPT           2u

static struct spinlock devices_lock = SPINLOCK("devices");
static struct hal_device xhci_kept[XHCI_KEPT];
static bool xhci_known[XHCI_KEPT];
static struct hal_device ether_kept[ETHERNET_KEPT];
static bool ether_known[ETHERNET_KEPT];
static struct hal_device ahci_kept[AHCI_KEPT];
static bool ahci_known[AHCI_KEPT];

static bool nth_ahci(unsigned index, struct pci_device *out)
{
    unsigned from = 0, at, seen = 0;

    while (pci_find_class(CLASS_STORAGE, SUBCLASS_SATA, from, out, &at)) {
        uint32_t word = pci_config_read(out->bus, out->slot, out->function,
                                        PCI_CLASS_WORD);

        if (((word >> 8) & 0xFFu) == INTERFACE_AHCI) {
            if (seen == index) {
                return true;
            }

            seen++;
        }

        from = at + 1;
    }

    return false;
}

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

/*
 * **An AMD GPU**: vendor 1002h and class 03h - VGA (0300h) for the card the
 * firmware shows the screen on, and possibly "other display" (0380h) for
 * the Mac Pro's second; matched by class alone, so both are found, and the
 * HD Audio function every Radeon carries beside it (class 04h) is not.
 * BAR 0 is the VRAM aperture and BAR 2 the registers on Southern Islands
 * (`radeon_device.c`, `amdgpu_device.c`), both 64-bit, which `pci_device`
 * already folds into bar[0] and bar[2].
 *
 * **Neither sized nor enabled**, for the Intel GPU's reason: the firmware's
 * screen is scanned out of one card's BAR 0 and the kernel console writes
 * there, and sizing a BAR turns the card's memory decoding off. The
 * register BAR's size is the 256 KB every SI card reports (from lspci, not
 * an AMD document - the driver maps only the first 76 KB of it); the
 * aperture's is left 0 and the driver reads it from the card.
 */
#define AMD                 0x1002u
#define AMD_GPU_KEPT        2u
#define AMD_VRAM_BAR        0u
#define AMD_REGS_BAR        2u
#define AMD_REGS_BYTES      0x40000u

static struct hal_device amd_kept[AMD_GPU_KEPT], amd_vram_kept[AMD_GPU_KEPT];
static bool amd_known[AMD_GPU_KEPT];

static bool nth_amd_gpu(unsigned index, struct pci_device *out)
{
    unsigned from = 0, at, seen = 0;

    while (pci_find(AMD, 0xFFFFu, from, out, &at)) {
        uint32_t word = pci_config_read(out->bus, out->slot, out->function,
                                        PCI_CLASS_WORD);

        if ((word >> 24) == CLASS_DISPLAY) {
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
 * **Broadcom Ethernet**, class 2 subclass 0 with Broadcom's vendor
 * identifier 14E4h: the 57765 class `tg3.c` drives - the Mac Pro 2013's two
 * BCM57762 ports and the iMac 2019's BCM57766. Subclass 0 only, so a Mac's
 * Broadcom wireless (class 0280h) is not taken for it. Which chip of the
 * family it is, the driver decides from the device id; the board finds
 * every Broadcom Ethernet controller, as it does Intel's.
 */
#define BROADCOM            0x14E4u
#define BROADCOM_KEPT       2u

static struct hal_device broadcom_kept[BROADCOM_KEPT];
static bool broadcom_known[BROADCOM_KEPT];

static bool nth_broadcom_ethernet(unsigned index, struct pci_device *out)
{
    unsigned from = 0, at, seen = 0;

    while (pci_find_class(CLASS_NETWORK, SUBCLASS_ETHERNET, from, out, &at)) {
        if (out->vendor == BROADCOM) {
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
/* GTTMMADR, and the device's identifier, when 0/2/0 is Intel's graphics. */
static bool intel_gpu_base(uint64_t *base, uint32_t *id_out)
{
    uint32_t id = pci_config_read(0, IGD_SLOT, 0, 0x00);
    uint32_t class_word = pci_config_read(0, IGD_SLOT, 0, PCI_CLASS_WORD);
    uint32_t low = pci_config_read(0, IGD_SLOT, 0, IGD_BAR0_LOW);

    if ((id & 0xFFFFu) != INTEL || (class_word >> 24) != CLASS_DISPLAY
        || (low & 0x7u) != IGD_BAR0_64BIT) {
        return false;
    }

    *base = ((uint64_t)pci_config_read(0, IGD_SLOT, 0, IGD_BAR0_HIGH) << 32)
          | (low & IGD_BASE_MASK);
    *id_out = id;

    return *base != 0;
}

/*
 * **The whole window**, for the display engine's driver (`m700-2d.md` step
 * 2): GTTMMADR's 16 MB, which the manual gives on Gen8 and later - not
 * sized, for the reason above.
 */
#define IGD_GTTMMADR_BYTES  0x1000000u

/*
 * **Its interrupt, an MSI taken once and kept** (`docs/gpu3d.md` G7e):
 * the driver arms the engines' interrupts and waits on this, so a fence is
 * heard when the GPU writes it rather than at the next tick. Taken the
 * first time the device is asked for and the same vector handed every
 * time after, as the controllers below keep theirs. Nought when it has
 * none, and the driver polls as before.
 */
static unsigned intel_gpu_interrupt(uint32_t id)
{
    static bool asked;
    static unsigned vector;
    unsigned long flags = spin_lock(&devices_lock);

    if (!asked) {
        struct pci_device pci = {
            .bus = 0, .slot = IGD_SLOT, .function = 0,
            .vendor = (uint16_t)id, .device = (uint16_t)(id >> 16),
        };

        asked = true;
        vector = pci_take_interrupt(&pci) ? pci.irq : 0u;
    }

    spin_unlock(&devices_lock, flags);
    return vector;
}

static bool intel_gpu(struct hal_device *out)
{
    uint64_t base;
    uint32_t id;

    if (!intel_gpu_base(&base, &id)) {
        return false;
    }

    out->base  = (unsigned long)base;
    out->size  = IGD_GTTMMADR_BYTES;
    out->intid = intel_gpu_interrupt(id);
    out->line  = id >> 16;
    out->where = IGD_SLOT << 3;
    out->id    = (id & 0xFFFFu) << 16 | id >> 16;

    return true;
}

static bool intel_backlight(struct hal_device *out)
{
    uint64_t base;
    uint32_t id;

    if (!intel_gpu_base(&base, &id)) {
        return false;
    }

    out->base  = (unsigned long)(base + BACKLIGHT_BLOCK);
    out->size  = BACKLIGHT_BYTES;
    out->intid = 0;
    out->line  = 0;
    out->where = IGD_SLOT << 3;
    out->id    = 0;

    return true;
}

bool hal_device_find(unsigned kind, unsigned index, struct hal_device *out)
{
    unsigned long flags;
    bool found = false;

    if (out != NULL && kind == HAL_DEV_INTEL_BACKLIGHT) {
        return index == 0 && intel_backlight(out);
    }

    if (out != NULL && kind == HAL_DEV_INTEL_GPU) {
        return index == 0 && intel_gpu(out);
    }

    if (out != NULL && (kind == HAL_DEV_AMD_GPU || kind == HAL_DEV_AMD_VRAM)
        && index < AMD_GPU_KEPT) {
        bool got = false;

        flags = spin_lock(&devices_lock);

        if (!amd_known[index]) {
            struct pci_device pci;

            if (nth_amd_gpu(index, &pci) && pci.bar[AMD_REGS_BAR] != 0) {
                unsigned where = ((unsigned)pci.bus << 8) | ((unsigned)pci.slot << 3)
                               | (unsigned)pci.function;
                unsigned id = ((unsigned)pci.vendor << 16) | pci.device;
                unsigned vector;

                /* Its MSI, taken once and kept as the Intel GPU's is, and
                 * bus mastering on - which the MSI needs and the GART
                 * (docs/amdgpu.md V1, V2); decoding left as the firmware
                 * left it. Nought when it has no MSI: the driver polls. */
                pci_set_master(&pci);
                vector = pci_take_interrupt(&pci) ? pci.irq : 0u;

                amd_kept[index] = (struct hal_device){
                    .base = (unsigned long)pci.bar[AMD_REGS_BAR], .size = AMD_REGS_BYTES,
                    .intid = vector, .line = pci.device, .where = where, .id = id };
                amd_vram_kept[index] = (struct hal_device){
                    .base = (unsigned long)pci.bar[AMD_VRAM_BAR], .size = 0,
                    .intid = 0, .line = pci.device, .where = where, .id = id };
                amd_known[index] = true;
            }
        }

        if (amd_known[index] && (kind == HAL_DEV_AMD_GPU || amd_vram_kept[index].base != 0)) {
            *out = kind == HAL_DEV_AMD_GPU ? amd_kept[index] : amd_vram_kept[index];
            got = true;
        }

        spin_unlock(&devices_lock, flags);
        return got;
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
                ether_kept[index].id    = ((unsigned)pci.vendor << 16)
                                        | pci.device;
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

    if (out != NULL && kind == HAL_DEV_BROADCOM_ETHERNET
        && index < BROADCOM_KEPT) {
        bool found = false;

        flags = spin_lock(&devices_lock);

        if (!broadcom_known[index]) {
            struct pci_device pci;
            uint64_t size;

            if (nth_broadcom_ethernet(index, &pci) && pci.bar[0] != 0
                && (size = pci_bar_size(&pci, 0)) != 0) {
                pci_enable(&pci);

                broadcom_kept[index].base  = (unsigned long)pci.bar[0];
                broadcom_kept[index].size  = (unsigned long)size;
                broadcom_kept[index].intid = pci.irq;
                broadcom_kept[index].line  = pci.device;
                broadcom_kept[index].where = ((unsigned)pci.bus << 8)
                                           | ((unsigned)pci.slot << 3)
                                           | (unsigned)pci.function;
                broadcom_kept[index].id    = ((unsigned)pci.vendor << 16)
                                           | pci.device;
                broadcom_known[index] = true;
            }
        }

        if (broadcom_known[index]) {
            *out = broadcom_kept[index];
            found = true;
        }

        spin_unlock(&devices_lock, flags);
        return found;
    }

    /* The same for a SATA controller, from its ABAR rather than BAR0. */
    if (out != NULL && kind == HAL_DEV_AHCI && index < AHCI_KEPT) {
        flags = spin_lock(&devices_lock);

        if (!ahci_known[index]) {
            struct pci_device pci;
            uint64_t size;

            if (nth_ahci(index, &pci) && pci.bar[AHCI_ABAR] != 0
                && (size = pci_bar_size(&pci, AHCI_ABAR)) != 0) {
                pci_enable(&pci);

                ahci_kept[index].base  = (unsigned long)pci.bar[AHCI_ABAR];
                ahci_kept[index].size  = (unsigned long)size;
                ahci_kept[index].intid = pci.irq;
                ahci_kept[index].line  = pci.device;
                ahci_kept[index].where = ((unsigned)pci.bus << 8)
                                       | ((unsigned)pci.slot << 3)
                                       | (unsigned)pci.function;
                ahci_kept[index].id    = ((unsigned)pci.vendor << 16)
                                       | pci.device;
                ahci_known[index] = true;
            }
        }

        if (ahci_known[index]) {
            *out = ahci_kept[index];
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
            xhci_kept[index].id    = ((unsigned)pci.vendor << 16)
                                   | pci.device;
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

/* Where on PCI a found and kept device is, or false. Called with devices_lock held. */
static bool kept_where(unsigned kind, unsigned index, unsigned *where)
{
    if (kind == HAL_DEV_INTEL_ETHERNET && index < ETHERNET_KEPT
        && ether_known[index]) {
        *where = ether_kept[index].where;
        return true;
    } else if (kind == HAL_DEV_XHCI && index < XHCI_KEPT && xhci_known[index]) {
        *where = xhci_kept[index].where;
        return true;
    } else if (kind == HAL_DEV_AHCI && index < AHCI_KEPT && ahci_known[index]) {
        *where = ahci_kept[index].where;
        return true;
    } else if (kind == HAL_DEV_AMD_GPU && index < AMD_GPU_KEPT && amd_known[index]) {
        *where = amd_kept[index].where;
        return true;
    } else if (kind == HAL_DEV_BROADCOM_ETHERNET && index < BROADCOM_KEPT
               && broadcom_known[index]) {
        *where = broadcom_kept[index].where;
        return true;
    }

    return false;
}

/*
 * **One word of a found device's configuration space** (`SYS_DEV_CONFIG`,
 * `testing.md` 18.347). Only of a device found and kept above - so only
 * where a driver already has it - and only read: the I219's
 * descriptor-ring status, which Linux's `e1000e` reads before it resets one,
 * and its command register, which says whether the card may use the bus.
 * The legacy mechanism's 256 bytes, a word at a time.
 */
bool hal_device_config(unsigned kind, unsigned index, unsigned offset,
                       uint32_t *out)
{
    unsigned long flags;
    unsigned where = 0;
    bool known;

    if (out == NULL || offset > 252u || (offset & 3u) != 0) {
        return false;
    }

    flags = spin_lock(&devices_lock);
    known = kept_where(kind, index, &where);
    spin_unlock(&devices_lock, flags);

    if (!known) {
        return false;
    }

    *out = pci_config_read((uint8_t)(where >> 8), (uint8_t)((where >> 3) & 0x1Fu),
                           (uint8_t)(where & 0x7u), (uint8_t)offset);
    return true;
}

/*
 * **One word of a found device's configuration space, written**
 * (`SYS_DEV_CONFIG_SET`). Only of a device found and kept above, only an
 * aligned word below 0x100, and only one a driver has business with:
 *
 *  - never 00h (identity), 08h (class), 0Ch (header type, BIST) or 10h-3Ch
 *    (BARs, subsystem, expansion ROM, capability pointer, interrupt line):
 *    the board placed the device and owns where it is;
 *  - the command register (04h) only with memory and bus master on and
 *    I/O off - what `pci_enable` set - so a driver can put back what a
 *    reset cleared and nothing else; the status half is RW1C, and a driver
 *    writes it as 0 or as the errors it means to clear;
 *  - never inside MSI's or MSI-X's capability (its header word included):
 *    the board chose the vector, and a driver that could rewrite it could
 *    raise somebody else's interrupt;
 *  - anything else - a chip's own registers above 40h, PCIe's Device
 *    Control, power management's control word - is the driver's.
 */
#define PCI_COMMAND_IO_BIT      0x1u
#define PCI_COMMAND_MEMORY_BIT  0x2u
#define PCI_COMMAND_MASTER_BIT  0x4u
#define CAP_ID_MSI_WRITE        0x05u
#define CAP_ID_MSIX_WRITE       0x11u

static bool in_capability(unsigned where, unsigned offset, uint8_t id,
                          unsigned bytes)
{
    uint8_t bus = (uint8_t)(where >> 8), slot = (uint8_t)((where >> 3) & 0x1Fu);
    uint8_t fn = (uint8_t)(where & 0x7u);
    unsigned at = pci_config_read(bus, slot, fn, 0x34) & 0xFCu, guard;

    for (guard = 0; at >= 0x40u && guard < 48u; guard++) {
        uint32_t cap = pci_config_read(bus, slot, fn, (uint8_t)at);

        if ((cap & 0xFFu) == id) {
            return offset >= at && offset < at + bytes;
        }

        at = (cap >> 8) & 0xFCu;
    }

    return false;
}

bool hal_device_config_write(unsigned kind, unsigned index, unsigned offset,
                             uint32_t value)
{
    unsigned long flags;
    unsigned where = 0;
    bool known;

    if (offset > 252u || (offset & 3u) != 0 || offset == 0x00u
        || offset == 0x08u || offset == 0x0Cu
        || (offset >= 0x10u && offset <= 0x3Cu)) {
        return false;
    }

    if (offset == 0x04u
        && ((value & PCI_COMMAND_MEMORY_BIT) == 0
            || (value & PCI_COMMAND_MASTER_BIT) == 0
            || (value & PCI_COMMAND_IO_BIT) != 0)) {
        return false;
    }

    flags = spin_lock(&devices_lock);
    known = kept_where(kind, index, &where);
    spin_unlock(&devices_lock, flags);

    if (!known
        || in_capability(where, offset, CAP_ID_MSI_WRITE, 24u)     /* 64-bit, masked: 24 bytes */
        || in_capability(where, offset, CAP_ID_MSIX_WRITE, 12u)) {
        return false;
    }

    pci_config_write((uint8_t)(where >> 8), (uint8_t)((where >> 3) & 0x1Fu),
                     (uint8_t)(where & 0x7u), (uint8_t)offset, value);
    return true;
}
