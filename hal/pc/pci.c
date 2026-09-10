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
#include "apic.h"
#include "pc.h"
#include "syscall.h"
#include "hda.h"
#include "snd.h"
#include "virtio.h"
#include "spinlock.h"

#define PCI_CONFIG_ADDR     0xCF8
#define PCI_CONFIG_DATA     0xCFC

#define PCI_VENDOR          0x00
#define PCI_COMMAND         0x04
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_CLASS            0x08
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D
#define PCI_CAP_POINTER     0x34
#define PCI_STATUS_REG      0x04
#define PCI_STATUS_CAPS     (1u << 20)

#define CAP_ID_MSI          0x05

/* Message Control, at +2 into the capability. */
#define MSI_ENABLE          (1u << 0)
#define MSI_64BIT           (1u << 7)

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

/*
 * **Two ports, one question.** A configuration read writes the address to
 * 0xCF8 and reads the answer at 0xCFC, and a second core doing the same
 * between the two would have this read answer its question. One lock for
 * both directions, interrupts masked, for two port operations.
 */
static struct spinlock pci_lock = SPINLOCK("pci");

uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t offset)
{
    unsigned long flags = spin_lock(&pci_lock);
    uint32_t value;

    out32(PCI_CONFIG_ADDR, address_of(bus, slot, fn, offset));
    value = in32(PCI_CONFIG_DATA);

    spin_unlock(&pci_lock, flags);
    return value;
}

void pci_config_write(uint8_t bus, uint8_t slot, uint8_t fn,
                      uint8_t offset, uint32_t value)
{
    unsigned long flags = spin_lock(&pci_lock);

    out32(PCI_CONFIG_ADDR, address_of(bus, slot, fn, offset));
    out32(PCI_CONFIG_DATA, value);

    spin_unlock(&pci_lock, flags);
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

/*
 * The line this device will actually arrive on.
 *
 * **Two different numbers depending on which controller is running, and
 * that is the part with no shortcut.** The Interrupt Line register at 0x3C
 * holds what the firmware assigned for the 8259 pair, and it is what the
 * legacy path wants. Under an I/O APIC that register means nothing: the
 * device's INTx pin goes to one of the chipset's four interrupt links, and
 * which input those links land on is described in ACPI's `_PRT` - which is
 * AML, and `acpi.h` says plainly that there is no interpreter here and
 * there is not going to be one.
 *
 * So the standard PCIe arrangement is computed instead: the four pins are
 * swizzled by device number so that the four functions of a slot do not all
 * land on the same link, and the links occupy the four inputs above the
 * sixteen legacy ones. `GSI = 16 + ((device + pin - 1) mod 4)`, which is
 * what q35 implements and what the PCI Express specification's routing
 * recommendation describes.
 *
 * **It is a convention rather than a promise**, and this is the one place
 * in the APIC path that could be wrong on a machine nobody has tried. A
 * board that routes differently would give a device that never interrupts -
 * which is why `hal_irq_describe` says which controller is running, and why
 * `opt/kosmos/irq=pic` exists to put a machine back on the legacy path
 * without a rebuild.
 */
static uint8_t interrupt_of(uint8_t bus, uint8_t slot, uint8_t fn)
{
    uint32_t word = pci_config_read(bus, slot, fn, PCI_INTERRUPT_LINE);
    uint8_t line = (uint8_t)(word & 0xFF);
    uint8_t pin = (uint8_t)((word >> 8) & 0xFF);

    if (!pc_irq_on_apic() || pin == 0 || pin > 4) {
        return line;            /* the legacy path, or a device with no pin */
    }

    return (uint8_t)(16u + ((slot + pin - 1u) & 3u));
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
                out->irq      = interrupt_of((uint8_t)bus, (uint8_t)slot,
                                             (uint8_t)fn);
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

/*
 * The vectors an MSI may be delivered on, and why they are a range of their
 * own.
 *
 * Inputs 16 to 19 of the I/O APIC are the chipset's four PCI links and are
 * reached by the swizzle in `interrupt_of`; these are above them. Both
 * arrangements exist at once because both are needed: a device with no MSI
 * capability still has to arrive somehow, and a chipset's own integrated
 * devices are exactly the ones whose link routing only AML describes.
 */
#define MSI_IRQ_FIRST   20u
#define MSI_IRQ_LAST    23u

static unsigned next_msi = MSI_IRQ_FIRST;

/*
 * Switches a device to Message Signalled Interrupts.
 *
 * **An MSI is not a wire.** The device writes a word to an address, and the
 * address is the local APIC's - so the interrupt arrives with a vector this
 * kernel chose, at a processor this kernel named, with nothing in between.
 * There is no routing table to consult, no `_PRT` in AML to interpret, and
 * no line shared with three other devices that each have to be asked
 * whether it was theirs.
 *
 * That last part is why this is the answer rather than a shortcut. The
 * I/O APIC path needs to know which of the chipset's four interrupt links a
 * device's pin lands on, and on an integrated device - the HDA controller
 * being exactly one - that is described only in AML. `acpi.h` says there is
 * no interpreter here. With MSI the question does not arise.
 *
 * Address and data, from the Intel SDM volume 3 §11.11: the address is
 * 0xFEE00000 with the destination's local APIC id in bits 19:12, and the
 * data is the vector, delivered as a fixed edge-triggered interrupt.
 *
 * False when the device has no MSI capability, when the APIC is not what
 * this machine is driving, or when the vectors have run out - and in every
 * one of those cases the caller keeps the line it already had.
 */
static bool msi_enable(struct pci_device *dev)
{
    uint8_t at;
    unsigned guard;
    unsigned irq;

    if (!pc_irq_on_apic() || next_msi > MSI_IRQ_LAST) {
        return false;
    }

    if ((pci_config_read(dev->bus, dev->slot, dev->function, PCI_STATUS_REG)
         & PCI_STATUS_CAPS) == 0) {
        return false;           /* no capability list at all */
    }

    at = (uint8_t)(pci_config_read(dev->bus, dev->slot, dev->function,
                                   PCI_CAP_POINTER) & 0xFC);

    /* Bounded: a capability list is a linked list in memory a device
     * controls, and a loop in it would be a hang at boot. */
    for (guard = 0; at != 0 && guard < 48u; guard++) {
        uint32_t head = pci_config_read(dev->bus, dev->slot, dev->function,
                                        at);

        if ((head & 0xFF) == CAP_ID_MSI) {
            break;
        }

        at = (uint8_t)((head >> 8) & 0xFC);
    }

    if (at == 0 || guard >= 48u) {
        return false;
    }

    irq = next_msi;

    {
        uint32_t control = pci_config_read(dev->bus, dev->slot, dev->function,
                                           at);
        uint16_t message = (uint16_t)(control >> 16);
        uint32_t address = 0xFEE00000u | ((uint32_t)apic_id() << 12);
        uint32_t data = PC_IRQ_BASE + irq;

        pci_config_write(dev->bus, dev->slot, dev->function, at + 4, address);

        /*
         * Where the data word sits depends on whether the device can take a
         * 64-bit address, because the upper half is only present when it
         * can. Getting this wrong writes the vector into the address.
         */
        if ((message & MSI_64BIT) != 0) {
            pci_config_write(dev->bus, dev->slot, dev->function, at + 8, 0);
            pci_config_write(dev->bus, dev->slot, dev->function, at + 12, data);
        } else {
            pci_config_write(dev->bus, dev->slot, dev->function, at + 8, data);
        }

        /*
         * One message, and enabled. Bits 6:4 are how many the device may
         * use and are left at zero: a driver that wanted a vector per queue
         * would ask for more, and none does.
         */
        message = (uint16_t)((message & ~(uint16_t)0x0070) | MSI_ENABLE);

        pci_config_write(dev->bus, dev->slot, dev->function, at,
                         (control & 0xFFFFu) | ((uint32_t)message << 16));
    }

    next_msi++;
    dev->irq = (uint8_t)irq;

    return true;
}

void pci_enable(struct pci_device *dev)
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

    /*
     * And an MSI if this machine and this device can both do one. `dev->irq`
     * changes when it can, which is the whole reason this takes a pointer.
     */
    (void)msi_enable(dev);
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
