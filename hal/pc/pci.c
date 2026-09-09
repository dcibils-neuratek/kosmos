/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The PCI bus, walked.
 *
 * `pci.h` says why this exists and what it deliberately does not do. What
 * is here is the smallest thing that can find a device and describe its
 * windows: the two-port configuration mechanism, a walk over every bus,
 * slot and function, and BAR decoding.
 *
 * PCI specification 3.0, and QEMU's q35 implements it faithfully enough
 * that nothing here is a workaround.
 */

#include <stddef.h>
#include <stdint.h>

#include "hal.h"
#include "pci.h"
#include "pc.h"
#include "syscall.h"
#include "hda.h"
#include "snd.h"
#include "virtio.h"

#define PCI_CONFIG_ADDR     0xCF8
#define PCI_CONFIG_DATA     0xCFC

#define PCI_VENDOR          0x00
#define PCI_COMMAND         0x04
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_CLASS            0x08
#define PCI_INTERRUPT_LINE  0x3C

#define COMMAND_IO          (1u << 0)
#define COMMAND_MEMORY      (1u << 1)
#define COMMAND_MASTER      (1u << 2)

#define BAR_IO              (1u << 0)
#define BAR_TYPE_MASK       (3u << 1)
#define BAR_TYPE_64         (2u << 1)

#define PCI_NONE            0xFFFFu

static void out32(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" :: "a"(value), "Nd"(port));
}

static uint32_t in32(uint16_t port)
{
    uint32_t v;

    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));

    return v;
}

/*
 * The address register, and bit 31 is what makes the pair mean anything.
 *
 * Without it the write is just a write to a port and the read that follows
 * returns whatever was last there - which looks exactly like a bus with one
 * device on it that is present at every address.
 */
static uint32_t address_of(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off)
{
    return 0x80000000u
         | ((uint32_t)bus  << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)fn   << 8)
         | (off & 0xFCu);
}

uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t offset)
{
    out32(PCI_CONFIG_ADDR, address_of(bus, slot, fn, offset));

    return in32(PCI_CONFIG_DATA);
}

void pci_config_write(uint8_t bus, uint8_t slot, uint8_t fn,
                      uint8_t offset, uint32_t value)
{
    out32(PCI_CONFIG_ADDR, address_of(bus, slot, fn, offset));
    out32(PCI_CONFIG_DATA, value);
}

/*
 * The six windows, decoded.
 *
 * Two things here are the specification rather than caution. A memory BAR
 * whose type field says 64-bit takes *two* slots, and the second holds the
 * high half - reading it as a window of its own gives a plausible address
 * that is really somebody's upper bits. And the low four bits of a memory
 * BAR are flags rather than address, so they come off; an I/O BAR uses the
 * low two and is not a memory window at all, which is why it is skipped.
 */
static void read_bars(struct pci_device *out)
{
    unsigned i;

    for (i = 0; i < 6; i++) {
        uint32_t low = pci_config_read(out->bus, out->slot, out->function,
                                       (uint8_t)(PCI_BAR0 + i * 4));

        out->bar[i] = 0;

        if (low == 0 || (low & BAR_IO) != 0) {
            continue;
        }

        if ((low & BAR_TYPE_MASK) == BAR_TYPE_64) {
            uint32_t high = pci_config_read(out->bus, out->slot, out->function,
                                            (uint8_t)(PCI_BAR0 + (i + 1) * 4));

            out->bar[i] = ((uint64_t)high << 32) | (low & ~0xFu);
            i++;                    /* the pair is one window */
        } else {
            out->bar[i] = low & ~0xFu;
        }
    }
}

/*
 * What a caller is looking for.
 *
 * Two ways to name a device and they are not interchangeable. A virtio
 * device is found by its vendor because that is what makes it virtio. **A
 * sound controller has to be found by its class**, because every chipset
 * gives its own the vendor's own identifier - QEMU's is 8086:2668 and Comet
 * Lake's is something else entirely - and a driver that matched on one of
 * them would work on exactly one machine.
 *
 * One scan either way. The alternative was a second copy of the walk below,
 * and `pc.h` says what this project thinks of a fact written twice.
 */
struct match {
    bool     by_class;
    uint16_t vendor, device;
    uint8_t  class, subclass;
};

static bool matches(const struct match *m, uint8_t bus, uint8_t slot,
                    uint8_t fn, uint16_t v, uint16_t d)
{
    if (m->by_class) {
        uint32_t cls = pci_config_read(bus, slot, fn, PCI_CLASS);

        return (uint8_t)(cls >> 24) == m->class
            && (uint8_t)(cls >> 16) == m->subclass;
    }

    if (v != m->vendor) {
        return false;
    }

    return m->device == PCI_NONE || d == m->device;
}

static bool find(const struct match *want, unsigned from,
                 struct pci_device *out, unsigned *found_at);

bool pci_find(uint16_t vendor, uint16_t device, unsigned from,
              struct pci_device *out, unsigned *found_at)
{
    struct match want = { false, vendor, device, 0, 0 };

    return find(&want, from, out, found_at);
}

bool pci_find_class(uint8_t class, uint8_t subclass, unsigned from,
                    struct pci_device *out, unsigned *found_at)
{
    struct match want = { true, 0, 0, class, subclass };

    return find(&want, from, out, found_at);
}

static bool find(const struct match *want, unsigned from,
                 struct pci_device *out, unsigned *found_at)
{
    unsigned seen = 0;
    unsigned bus;

    /*
     * Every bus, every slot, every function, in order.
     *
     * Two hundred and fifty-six buses is the whole address space and QEMU's
     * q35 uses one of them; walking all of it costs about half a million
     * port reads at boot and finds a device that is not on bus zero if
     * anything ever puts one there. The alternative is following bridges,
     * which is a recursion and a header-type test for a machine that has
     * one bus.
     */
    for (bus = 0; bus < 256; bus++) {
        unsigned slot;

        for (slot = 0; slot < 32; slot++) {
            unsigned fn;
            unsigned functions = 1;

            for (fn = 0; fn < functions; fn++) {
                uint32_t id = pci_config_read((uint8_t)bus, (uint8_t)slot,
                                              (uint8_t)fn, PCI_VENDOR);
                uint16_t v = (uint16_t)(id & 0xFFFF);
                uint16_t d = (uint16_t)(id >> 16);

                if (v == PCI_NONE) {
                    continue;
                }

                /*
                 * Bit 7 of the header type says the device has more than
                 * one function. Asked only of function zero, because that
                 * is the only one guaranteed to exist - and a device that
                 * says nothing is a device with exactly one.
                 */
                if (fn == 0) {
                    uint32_t word = pci_config_read((uint8_t)bus,
                                                    (uint8_t)slot, 0,
                                                    PCI_HEADER_TYPE);

                    if ((((word >> 16) & 0xFF) & 0x80) != 0) {
                        functions = 8;
                    }
                }

                if (!matches(want, (uint8_t)bus, (uint8_t)slot,
                             (uint8_t)fn, v, d)) {
                    continue;
                }

                if (seen++ < from) {
                    continue;
                }

                out->bus      = (uint8_t)bus;
                out->slot     = (uint8_t)slot;
                out->function = (uint8_t)fn;
                out->vendor   = v;
                out->device   = d;
                out->irq      = (uint8_t)(pci_config_read((uint8_t)bus,
                                                          (uint8_t)slot,
                                                          (uint8_t)fn,
                                                          PCI_INTERRUPT_LINE)
                                          & 0xFF);
                read_bars(out);

                if (found_at != NULL) {
                    *found_at = seen - 1;
                }

                return true;
            }
        }
    }

    return false;
}

void pci_enable(const struct pci_device *dev)
{
    uint32_t command = pci_config_read(dev->bus, dev->slot, dev->function,
                                       PCI_COMMAND);

    /*
     * Memory decoding, so a BAR answers at all, and bus mastering, so the
     * device may fetch the descriptor rings the driver hands it. Both are
     * off out of reset, and a virtio device with mastering off accepts
     * every buffer, raises no interrupt, and never says why.
     */
    command |= COMMAND_MEMORY | COMMAND_MASTER;
    command &= ~(uint32_t)COMMAND_IO;

    pci_config_write(dev->bus, dev->slot, dev->function, PCI_COMMAND, command);
}

/*
 * Whether one of this system's drivers took this device.
 *
 * Two drivers can claim something. The sound controller is found by its
 * class, so it is answered by its class here too - and it is the only
 * device in this system that is neither virtio nor firmware.
 *
 * For virtio the question reduces to: is this a virtio device, of which
 * type, and did that driver come up? The type is not simply the device id -
 * a *transitional* device answers in the legacy range and puts its type in
 * the subsystem id instead, which is the same trap `is_kind` in `virtio.c`
 * documents and the reason `virtio-net-pci` was invisible until it was
 * handled.
 *
 * Everything else on a q35 - the host bridge, the ISA bridge, the SATA
 * controller QEMU puts there whether or not a disk is attached - is
 * genuinely undriven, and saying so is the point of this file's new
 * function.
 */
static uint8_t claimed_here(uint8_t slot, uint8_t fn, uint32_t id)
{
    uint16_t vendor = (uint16_t)(id & 0xFFFF);
    uint16_t device = (uint16_t)((id >> 16) & 0xFFFF);
    uint32_t cls = pci_config_read(0, slot, fn, PCI_CLASS);
    uint32_t type;

    if ((uint8_t)(cls >> 24) == 0x04 && (uint8_t)(cls >> 16) == 0x03) {
        return hda_present() ? 1u : 0u;
    }

    if (vendor != 0x1AF4) {
        return 0;                       /* not virtio; nothing here drives it */
    }

    if (device >= 0x1040) {
        type = device - 0x1040u;
    } else {
        type = (pci_config_read(0, slot, fn, 0x2C) >> 16) & 0xFFFF;
    }

    switch (type) {
    case 1:  return hal_net_present()      ? 1u : 0u;   /* virtio-net */
    case 2:  return hal_blk_present()      ? 1u : 0u;   /* virtio-blk */
    case 18: return keyboard_present()     ? 1u : 0u;   /* virtio-input */
    case 25: return virtio_snd_present()      ? 1u : 0u;   /* virtio-sound */
    default: return 0;
    }
}

/*
 * Everything on the bus, and whether we drive it.
 *
 * `pci_find` answers "is there one of these", which is what a driver wants
 * and cannot answer the question a person asks: a slot holding a device
 * nothing claims is invisible to it. This walks the whole of bus 0 instead
 * and reports what is there either way.
 *
 * Bus 0 only, and function 0 of each slot unless the device says it is
 * multi-function. That is every device QEMU's q35 puts in front of us, and
 * a machine with a bridge to walk behind would need the recursion this
 * deliberately does not have - there is no such machine here yet, and
 * `hal.md` says not to write the interface before the second caller.
 */
unsigned hal_bus_scan(struct bus_device *out, unsigned max)
{
    unsigned n = 0;
    uint8_t slot;

    for (slot = 0; slot < 32 && n < max; slot++) {
        uint8_t fn, functions = 1;

        for (fn = 0; fn < functions && n < max; fn++) {
            uint32_t id = pci_config_read(0, slot, fn, 0x00);
            uint32_t cls;

            if ((id & 0xFFFF) == 0xFFFF) {
                continue;                       /* nothing in this function */
            }

            /* Header type bit 7: this slot has more than one function, so
             * the other seven are worth reading. Asked once, on function 0,
             * because that is the only one required to answer. */
            if (fn == 0) {
                uint32_t hdr = pci_config_read(0, slot, 0, 0x0C);

                if (((hdr >> 16) & 0x80) != 0) {
                    functions = 8;
                }
            }

            cls = pci_config_read(0, slot, fn, 0x08);

            out[n].id       = ((id & 0xFFFF) << 16) | ((id >> 16) & 0xFFFF);
            out[n].class    = cls >> 8;         /* class/subclass/prog-if */
            out[n].where    = (uint16_t)((slot << 3) | fn);
            out[n].claimed  = claimed_here(slot, fn, id);
            out[n].reserved = 0;
            n++;
        }
    }

    return n;
}
