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

#include "pci.h"
#include "pc.h"

#define PCI_CONFIG_ADDR     0xCF8
#define PCI_CONFIG_DATA     0xCFC

#define PCI_VENDOR          0x00
#define PCI_COMMAND         0x04
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
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

bool pci_find(uint16_t vendor, uint16_t device, unsigned from,
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

                if (v != vendor) {
                    continue;
                }

                if (device != PCI_NONE && d != device) {
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
