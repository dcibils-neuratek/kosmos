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
#include "mmio.h"
#include "mmu.h"
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

/* The first of the four I/O APIC inputs q35 routes its PCI links to. */
#define PCI_LINK_FIRST      20u
#define PCI_INTERRUPT_PIN   0x3D

/*
 * A PCI-to-PCI bridge's bus numbers start at 18h - primary, secondary,
 * subordinate - and its header type's layout code is 01h. From the bridge
 * header as gnu-efi's `pci22.h` lays it out (`PCI_BRIDGE_CONTROL_REGISTER`,
 * `HEADER_TYPE_PCI_TO_PCI_BRIDGE`); the PCI specification is not in this
 * project's references.
 */
#define PCI_BUS_NUMBERS     0x18
#define HEADER_LAYOUT       0x7Fu
#define HEADER_BRIDGE       0x01u
#define PCI_CAP_POINTER     0x34
#define PCI_STATUS_REG      0x04
#define PCI_STATUS_CAPS     (1u << 20)

#define CAP_ID_MSI          0x05

/* Message Control, at +2 into the capability. */
#define MSI_ENABLE          (1u << 0)
#define MSI_64BIT           (1u << 7)

/*
 * MSI-X. The capability's shape - ID 11h, the table's offset in 31:3 of its
 * second word and its BAR in 2:0 - and a table entry's sixteen bytes of
 * address, upper address, data and vector control are xHCI 1.2 5.2.8.2 and
 * 5.2.8.5. The three bits below are PCI 6.8.2's, which `msix_enable` says
 * more about.
 */
#define CAP_ID_MSIX         0x11
#define MSIX_ENABLE         (1u << 15)
#define MSIX_FUNCTION_MASK  (1u << 14)
#define MSIX_VECTOR_MASKED  (1u << 0)
#define MSIX_BIR_MASK       0x7u
#define MSIX_ENTRY_BYTES    16u
#define MSIX_ENTRY_ADDRESS  0u
#define MSIX_ENTRY_UPPER    4u
#define MSIX_ENTRY_DATA     8u
#define MSIX_ENTRY_CONTROL  12u

#define COMMAND_IO          (1u << 0)
#define COMMAND_MEMORY      (1u << 1)
#define COMMAND_MASTER      (1u << 2)

/*
 * **Interrupt Disable**, which stops a device raising its legacy INTx and
 * says nothing else. Firmware sets it on devices it has finished with, and
 * a device left on INTx with it set is one that works perfectly and never
 * interrupts - which reads as a driver that is polling, because it is.
 *
 * Found on 23 September with the Intel Ethernet driver: its frames arrived
 * on the driver's deadline rather than on its interrupt, so a ping over a
 * gigabit link came back in 103 milliseconds.
 */
#define COMMAND_INTX_OFF    (1u << 10)

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

uint64_t pci_bar_size(const struct pci_device *dev, unsigned index)
{
    uint8_t bus = dev->bus, slot = dev->slot, fn = dev->function;
    uint8_t at;
    uint32_t command, low, high, mask_low, mask_high = 0xFFFFFFFFu;
    uint64_t mask;
    bool wide;

    if (index >= 6) {
        return 0;
    }

    at = (uint8_t)(PCI_BAR0 + index * 4);
    low = pci_config_read(bus, slot, fn, at);

    if (low == 0 || (low & BAR_IO) != 0) {
        return 0;
    }

    wide = (low & BAR_TYPE_MASK) == BAR_TYPE_64;

    if (wide && index >= 5) {
        return 0;               /* a 64-bit BAR with no slot for its top half */
    }

    command = pci_config_read(bus, slot, fn, PCI_COMMAND);
    pci_config_write(bus, slot, fn, PCI_COMMAND,
                     command & ~(uint32_t)(COMMAND_MEMORY | COMMAND_IO));

    pci_config_write(bus, slot, fn, at, 0xFFFFFFFFu);
    mask_low = pci_config_read(bus, slot, fn, at);
    pci_config_write(bus, slot, fn, at, low);

    if (wide) {
        high = pci_config_read(bus, slot, fn, (uint8_t)(at + 4));
        pci_config_write(bus, slot, fn, (uint8_t)(at + 4), 0xFFFFFFFFu);
        mask_high = pci_config_read(bus, slot, fn, (uint8_t)(at + 4));
        pci_config_write(bus, slot, fn, (uint8_t)(at + 4), high);
    }

    pci_config_write(bus, slot, fn, PCI_COMMAND, command);

    /*
     * The bits that stayed set are the address; the ones that read back zero
     * are the size. A 32-bit BAR has no upper half to ask, and all ones there
     * is exactly what a 32-bit window means.
     */
    mask = ((uint64_t)mask_high << 32) | (mask_low & ~0xFu);

    if ((mask_low & ~0xFu) == 0 && (!wide || mask_high == 0)) {
        return 0;
    }

    return ~mask + 1u;
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
 * So the arrangement q35 actually implements is computed instead. The four
 * pins are swizzled by device number so that the four functions of a slot
 * do not all land on the same link, and the links sit on the *upper* four
 * of the chipset's eight: `GSI = 20 + ((device + pin - 1) mod 4)`.
 *
 * **The 20 is measured, not derived**, and it replaced a 16 that had been
 * reasoned to. An 82540EM was walked across four slots with every one of
 * the eight links claimed at once, and it asserted 22, 23, 20, 21 from
 * slots 2, 3, 4 and 5 - PIRQ E to H, period four, exactly as written above.
 * The 16 had been a reading of the PCI Express routing recommendation, and
 * it named four inputs the card never touched, so the driver woke on its
 * own deadline and a ping took 103 ms instead of 0.6.
 *
 * **It is still a convention rather than a promise**, and this is the one
 * place in the APIC path that could be wrong on a machine nobody has tried:
 * what a real chipset does is in its `_PRT`, which is AML, and `acpi.h` says
 * plainly that there is no interpreter here. A board that routes
 * differently gives a device that never interrupts - which is why
 * `hal_irq_describe` says which controller is running, and why
 * `opt/kosmos/irq=pic` exists to put a machine back on the legacy path
 * without a rebuild.
 *
 * It bites less than it reads: a device with an MSI capability never comes
 * here at all, and on a machine recent enough to have no 8259 pair, very
 * nearly everything has one.
 */
static uint8_t interrupt_of(uint8_t bus, uint8_t slot, uint8_t fn)
{
    uint32_t word = pci_config_read(bus, slot, fn, PCI_INTERRUPT_LINE);
    uint8_t line = (uint8_t)(word & 0xFF);
    uint8_t pin = (uint8_t)((word >> 8) & 0xFF);

    if (!pc_irq_on_apic() || pin == 0 || pin > 4) {
        return line;            /* the legacy path, or a device with no pin */
    }

    return (uint8_t)(PCI_LINK_FIRST + ((slot + pin - 1u) & 3u));
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
 * The numbers an MSI may be delivered on, and why they are a range of their
 * own.
 *
 * The I/O APIC's inputs are the chipset's lines - the sixteen ISA ones and
 * the PCI links above them, reached by the swizzle in `interrupt_of`. An
 * MSI is none of those: the device writes to the local APIC itself, so its
 * number only has to be a number nothing else uses, and everything above
 * the last input is free.
 *
 * **Asked of `apic.c` rather than agreed with it**, and that is the whole
 * of the fix. This said 20 and 23, with a comment claiming the links were
 * inputs 16 to 19. On q35 they are 20 to 23, so the four numbers this
 * handed out were also four real inputs - and `apic_unmask` read them as
 * MSIs and left those inputs masked for ever. An Ethernet card routed to
 * one of them asserted its line into an entry nobody had written, and the
 * only symptom was a driver that woke on its own deadline.
 *
 * Both arrangements exist at once because both are needed: a device with
 * no MSI capability still has to arrive somehow.
 */
static unsigned next_msi;
static unsigned last_msi;

/*
 * Filled on first use rather than at build time, because the count of
 * inputs comes from the I/O APIC's version register and that is read when
 * the machine boots.
 */
static void msi_range(void)
{
    if (next_msi == 0) {
        next_msi = apic_msi_first();
        last_msi = apic_msi_last();
    }
}

/*
 * Where capability `id` is in a device's list, or 0 when it has none.
 *
 * Bounded: a capability list is a linked list in memory a device controls,
 * and a loop in it would be a hang at boot. One walk for MSI and MSI-X both,
 * rather than a copy each.
 */
static uint8_t capability_at(const struct pci_device *dev, uint8_t id)
{
    uint8_t at;
    unsigned guard;

    if ((pci_config_read(dev->bus, dev->slot, dev->function, PCI_STATUS_REG)
         & PCI_STATUS_CAPS) == 0) {
        return 0;               /* no capability list at all */
    }

    at = (uint8_t)(pci_config_read(dev->bus, dev->slot, dev->function,
                                   PCI_CAP_POINTER) & 0xFC);

    for (guard = 0; at != 0 && guard < 48u; guard++) {
        uint32_t head = pci_config_read(dev->bus, dev->slot, dev->function,
                                        at);

        if ((head & 0xFF) == id) {
            return at;
        }

        at = (uint8_t)((head >> 8) & 0xFC);
    }

    return 0;
}

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
    unsigned irq;

    msi_range();

    if (!pc_irq_on_apic() || next_msi == 0 || next_msi > last_msi) {
        return false;
    }

    at = capability_at(dev, CAP_ID_MSI);

    if (at == 0) {
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

/*
 * **MSI-X, for a device that has no MSI capability.** QEMU's xHCI controller
 * is one, and that was measured rather than assumed: its capability list is
 * MSI-X at 90h and PCI Express at A0h and nothing else, so `msi_enable` found
 * nothing and the controller was left on a line no interrupt arrived on.
 *
 * The message is MSI's - the local APIC's address with its id in 19:12, the
 * vector as data (Intel SDM vol. 3 11.11) - written into entry 0 of a table
 * the device keeps in one of its BARs, rather than into configuration space.
 * Entry 0 is all a driver here needs: xHCI's interrupter 0 is its entry 0.
 *
 * In order: the function masked and MSI-X enabled, so nothing fires from a
 * half-written entry; the entry written and its own mask cleared; the
 * function unmasked.
 *
 * **Where the three bits come from.** The capability's shape and the entry's
 * layout are xHCI 1.2's (5.2.8.2, 5.2.8.5). Enable at bit 15 and Function
 * Mask at bit 14 of Message Control, and a vector's mask at bit 0 of Vector
 * Control, are the PCI specification's 6.8.2, **which is not in this
 * project's references**. What says they are right is an interrupt arriving:
 * `run_x86.py`'s USB check fails when the controller's No-Op is answered and
 * no interrupt reached the driver.
 */
static bool msix_enable(struct pci_device *dev)
{
    uint32_t control, table;
    uint16_t message;
    uintptr_t entry;
    unsigned bir, irq;
    uint8_t at;

    msi_range();

    if (!pc_irq_on_apic() || next_msi == 0 || next_msi > last_msi) {
        return false;
    }

    at = capability_at(dev, CAP_ID_MSIX);

    if (at == 0) {
        return false;
    }

    table = pci_config_read(dev->bus, dev->slot, dev->function,
                            (uint8_t)(at + 4));
    bir = table & MSIX_BIR_MASK;

    if (bir >= 6 || dev->bar[bir] == 0) {
        return false;
    }

    entry = mmu_map_device((uintptr_t)dev->bar[bir]
                           + (table & ~MSIX_BIR_MASK), MSIX_ENTRY_BYTES);

    if (entry == 0) {
        return false;           /* the device window is full */
    }

    irq = next_msi;
    control = pci_config_read(dev->bus, dev->slot, dev->function, at);
    message = (uint16_t)(control >> 16);

    pci_config_write(dev->bus, dev->slot, dev->function, at,
                     (control & 0xFFFFu)
                     | ((uint32_t)(message | MSIX_FUNCTION_MASK | MSIX_ENABLE)
                        << 16));

    mmio_write32(entry + MSIX_ENTRY_ADDRESS,
                 0xFEE00000u | ((uint32_t)apic_id() << 12));
    mmio_write32(entry + MSIX_ENTRY_UPPER, 0);
    mmio_write32(entry + MSIX_ENTRY_DATA, PC_IRQ_BASE + irq);
    mmio_write32(entry + MSIX_ENTRY_CONTROL,
                 mmio_read32(entry + MSIX_ENTRY_CONTROL) & ~MSIX_VECTOR_MASKED);

    pci_config_write(dev->bus, dev->slot, dev->function, at,
                     (control & 0xFFFFu)
                     | ((uint32_t)(uint16_t)((message | MSIX_ENABLE)
                                             & ~MSIX_FUNCTION_MASK) << 16));

    next_msi++;
    dev->irq = (uint8_t)irq;

    return true;
}

/*
 * **Which devices a driver took**, by address, for `hal_bus_scan`.
 *
 * `pci_enable` is the one call every driver on this board makes when it
 * takes a device - virtio's, the NVMe drive's, the sound controller's, and
 * the xHCI controllers the board hands to a process - so it is where "driven"
 * is written down. It was a function that knew each driver instead,
 * `claimed_here`, and it knew virtio and one class of sound controller: on
 * the ThinkPad every device on the bus read "NO DRIVER", the two xHCI
 * controllers a process was driving among them.
 *
 * **Taken is not the same as working.** A driver that enabled a device and
 * then failed with it has still taken it, and its line in the boot log says
 * why. And a full table is a seventeenth device listed as undriven - a wrong
 * row in a report, never a driver refused.
 */
#define TAKEN_MAX   16u

static struct spinlock taken_lock = SPINLOCK("pci taken");
static uint16_t taken[TAKEN_MAX];
static unsigned taken_count;

static uint16_t where_of(uint8_t bus, uint8_t slot, uint8_t function)
{
    return (uint16_t)(((unsigned)bus << 8) | ((unsigned)slot << 3)
                      | (unsigned)function);
}

static void take(const struct pci_device *dev)
{
    uint16_t where = where_of(dev->bus, dev->slot, dev->function);
    unsigned long flags = spin_lock(&taken_lock);
    unsigned i = 0;

    while (i < taken_count && taken[i] != where) {
        i++;
    }

    if (i == taken_count && taken_count < TAKEN_MAX) {
        taken[taken_count++] = where;
    }

    spin_unlock(&taken_lock, flags);
}

static bool was_taken(uint16_t where)
{
    unsigned long flags = spin_lock(&taken_lock);
    bool found = false;
    unsigned i;

    for (i = 0; i < taken_count && !found; i++) {
        found = taken[i] == where;
    }

    spin_unlock(&taken_lock, flags);
    return found;
}

void pci_enable(struct pci_device *dev)
{
    uint32_t command = pci_config_read(dev->bus, dev->slot, dev->function,
                                       PCI_COMMAND);

    take(dev);

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
     * And an MSI if this machine and this device can both do one, or MSI-X
     * where the device has only that. `dev->irq` changes when it can, which
     * is the whole reason this takes a pointer.
     */
    if (!msi_enable(dev) && !msix_enable(dev)) {
        /*
         * **Left on its legacy line, so the line has to be let through.**
         * A device the firmware finished with may have Interrupt Disable
         * set, and with it set the device raises nothing. Cleared only in
         * this branch: a device switched to MSI disables INTx itself, and
         * clearing the bit then would be asking for both.
         */
        command = pci_config_read(dev->bus, dev->slot, dev->function,
                                  PCI_COMMAND);

        if ((command & COMMAND_INTX_OFF) != 0) {
            pci_config_write(dev->bus, dev->slot, dev->function, PCI_COMMAND,
                             command & ~(uint32_t)COMMAND_INTX_OFF);
        }
    }
}


/*
 * Everything on the bus, and whether we drive it - **every bus a bridge
 * leads to, and not bus 0 alone.**
 *
 * `pci_find` answers "is there one of these", which is what a driver wants
 * and cannot answer the question a person asks: a slot holding a device
 * nothing claims is invisible to it. This walks the buses and reports what is
 * there either way, each device's bus in the high byte of `where`.
 *
 * It walked bus 0 and nothing more, under a comment that a machine with a
 * bridge to walk behind would need a recursion this did not have, "and there
 * is no such machine here yet". The ThinkPad is one - its drive is behind a
 * PCI Express root port - and This Machine listed twenty-two devices there
 * with no drive among them.
 *
 * **Followed, and not recursed.** A bridge says which bus is behind it, and
 * each bus reached that way is marked and walked in turn, in order: a chain
 * of bridges is a loop here rather than a recursion on a kernel stack. A
 * bridge whose secondary bus is not above its own is one the firmware did
 * not number, and is not followed.
 *
 * **The count is of everything found**, and the first `max` are written.
 * `find` walks every bus number blind instead, which is half a million reads
 * once, when a driver starts; this is asked whenever a program wants the
 * list, and costs a bus's worth of reads for each bus that exists.
 */
unsigned hal_bus_scan(struct bus_device *out, unsigned max)
{
    uint8_t reached[32] = { 1 };        /* a bit a bus, and bus 0 to start */
    unsigned n = 0;
    unsigned bus;

    for (bus = 0; bus < 256; bus++) {
        uint8_t slot;

        if ((reached[bus >> 3] & (1u << (bus & 7))) == 0) {
            continue;
        }

        for (slot = 0; slot < 32; slot++) {
            uint8_t fn, functions = 1;

            for (fn = 0; fn < functions; fn++) {
                uint32_t id = pci_config_read((uint8_t)bus, slot, fn, 0x00);
                uint32_t type;
                uint16_t where;

                if ((id & 0xFFFF) == 0xFFFF) {
                    continue;                   /* nothing in this function */
                }

                type = (pci_config_read((uint8_t)bus, slot, fn, 0x0C) >> 16)
                       & 0xFF;

                /* Header type bit 7: this slot has more than one function, so
                 * the other seven are worth reading. Asked of function 0,
                 * because that is the only one required to answer. */
                if (fn == 0 && (type & 0x80) != 0) {
                    functions = 8;
                }

                if ((type & HEADER_LAYOUT) == HEADER_BRIDGE) {
                    unsigned secondary = (pci_config_read((uint8_t)bus, slot,
                                                          fn, PCI_BUS_NUMBERS)
                                          >> 8) & 0xFF;

                    if (secondary > bus) {
                        reached[secondary >> 3] |=
                            (uint8_t)(1u << (secondary & 7));
                    }
                }

                where = where_of((uint8_t)bus, slot, fn);

                if (n < max) {
                    out[n].id       = ((id & 0xFFFF) << 16)
                                    | ((id >> 16) & 0xFFFF);
                    out[n].class    = pci_config_read((uint8_t)bus, slot, fn,
                                                      0x08) >> 8;
                    out[n].where    = where;
                    out[n].claimed  = was_taken(where) ? 1u : 0u;
                    out[n].reserved = 0;
                }

                n++;
            }
        }
    }

    return n;
}
