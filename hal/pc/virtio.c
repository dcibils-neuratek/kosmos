/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The virtio-pci conversation, which is the same conversation in a
 * different building.
 *
 * `hal/virtio/virtio.h` says what this has to provide and why the four
 * drivers above it need nothing else. `hal/qemu-virt/virtio.c` is the other
 * implementation, and comparing them is the clearest statement of what a
 * transport is: the *sequence* is identical - acknowledge, agree features,
 * attach queues, say ready - and every register it touches is somewhere
 * else.
 *
 * **virtio-mmio puts every register at a fixed offset from one window.**
 * virtio-pci scatters them: a common configuration structure, a notify
 * area, an interrupt status byte and the device's own configuration each
 * live somewhere in some BAR, and the only way to find them is to walk the
 * device's PCI capability list looking for vendor-specific entries that say
 * where. That walk is the whole of what this file has that the other does
 * not.
 *
 * Virtio 1.1 specification, section 4.1.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mmio.h"
#include "mmu.h"
#include "pci.h"
#include "pc.h"
#include "virtio.h"

/* virtio's own vendor id, and the device id it uses for modern devices:
 * 0x1040 plus the virtio device number. There is a transitional range at
 * 0x1000 for devices that also speak the legacy interface; QEMU offers both
 * ids for the same device and this asks for the modern one, which is the
 * same choice the ARM board makes by refusing legacy virtio-mmio. */
#define VIRTIO_PCI_VENDOR   0x1AF4
#define VIRTIO_PCI_MODERN   0x1040
#define VIRTIO_PCI_LEGACY   0x1000
#define PCI_SUBSYSTEM       0x2C
#define PCI_ANY_DEVICE      0xFFFFu

/* PCI capability ids and the virtio structure types, from the spec. */
#define CAP_VENDOR          0x09

#define VIRTIO_CAP_COMMON   1
#define VIRTIO_CAP_NOTIFY   2
#define VIRTIO_CAP_ISR      3
#define VIRTIO_CAP_DEVICE   4

/*
 * The common configuration structure, at offsets from its own base. This is
 * the register file virtio-mmio spreads over its window, packed tighter and
 * with the queue addresses as single 64-bit fields rather than pairs.
 */
#define COMMON_DEVICE_FEATURE_SEL   0x00
#define COMMON_DEVICE_FEATURE       0x04
#define COMMON_DRIVER_FEATURE_SEL   0x08
#define COMMON_DRIVER_FEATURE       0x0C
#define COMMON_NUM_QUEUES           0x12
#define COMMON_DEVICE_STATUS        0x14
#define COMMON_QUEUE_SELECT         0x16
#define COMMON_QUEUE_SIZE           0x18
#define COMMON_QUEUE_ENABLE         0x1C
#define COMMON_QUEUE_NOTIFY_OFF     0x1E
#define COMMON_QUEUE_DESC           0x20
#define COMMON_QUEUE_DRIVER         0x28
#define COMMON_QUEUE_DEVICE         0x30

#define PCI_STATUS          0x06
#define PCI_CAP_POINTER     0x34
#define PCI_STATUS_CAP_LIST (1u << 4)

/*
 * Eight- and sixteen-bit accessors, which virtio-mmio never needed.
 *
 * Its registers are all thirty-two bits wide; several of these are not -
 * `device_status` is a byte and `queue_size` is a halfword - and a device
 * is entitled to decode the width. Reading a byte field as a word works on
 * QEMU and is the kind of thing that stops working on hardware.
 */
static uint8_t read8(uintptr_t at)
{
    return mmio_read8(at);
}

static void write8(uintptr_t at, uint8_t value)
{
    mmio_write8(at, value);
}

static uint16_t read16(uintptr_t at)
{
    uint16_t v;

    __asm__ volatile("movw (%1), %0" : "=r"(v) : "r"(at) : "memory");

    return v;
}

static void write16(uintptr_t at, uint16_t value)
{
    __asm__ volatile("movw %0, (%1)" :: "r"(value), "r"(at) : "memory");
}

static void write64(uintptr_t at, uint64_t value)
{
    __asm__ volatile("movq %0, (%1)" :: "r"(value), "r"(at) : "memory");
}

/*
 * Walk the capability list for one of virtio's four structures and map it.
 *
 * Each entry is a vendor-specific capability whose body says which BAR the
 * structure is in and where in it - so this is two lookups: the capability
 * chain in configuration space, then the BAR the device already reported.
 *
 * The notify capability carries one field more than the others, a
 * multiplier for the distance between one queue's doorbell and the next,
 * and the caller asks for it separately.
 */
static uintptr_t find_structure(const struct pci_device *pci, uint8_t type,
                                uint32_t *multiplier)
{
    uint8_t at = (uint8_t)(pci_config_read(pci->bus, pci->slot, pci->function,
                                           PCI_CAP_POINTER) & 0xFC);
    unsigned guard;

    if ((pci_config_read(pci->bus, pci->slot, pci->function, PCI_STATUS)
         & (PCI_STATUS_CAP_LIST << 16)) == 0) {
        return 0;
    }

    /* Bounded: a capability list is a linked list in memory a device
     * controls, and a device with a loop in it must not be a machine that
     * hangs at boot. Forty-eight is more entries than the space holds. */
    for (guard = 0; at != 0 && guard < 48; guard++) {
        uint32_t head = pci_config_read(pci->bus, pci->slot, pci->function, at);
        uint8_t id    = (uint8_t)(head & 0xFF);
        uint8_t next  = (uint8_t)((head >> 8) & 0xFF);
        uint8_t kind  = (uint8_t)((head >> 24) & 0xFF);

        if (id == CAP_VENDOR && kind == type) {
            uint32_t which = pci_config_read(pci->bus, pci->slot,
                                             pci->function,
                                             (uint8_t)(at + 4)) & 0xFF;
            uint32_t offset = pci_config_read(pci->bus, pci->slot,
                                              pci->function,
                                              (uint8_t)(at + 8));
            uint32_t length = pci_config_read(pci->bus, pci->slot,
                                              pci->function,
                                              (uint8_t)(at + 12));

            if (which >= 6 || pci->bar[which] == 0 || length == 0) {
                return 0;
            }

            if (multiplier != NULL) {
                *multiplier = pci_config_read(pci->bus, pci->slot,
                                              pci->function,
                                              (uint8_t)(at + 16));
            }

            return mmu_map_device((uintptr_t)pci->bar[which] + offset, length);
        }

        at = (uint8_t)(next & 0xFC);
    }

    return 0;
}

/*
 * Whether this PCI device is the virtio device the caller asked for, and
 * **there are two ways for it to be.**
 *
 * A device that speaks only virtio 1.0 answers at 0x1040 plus its type,
 * which is the tidy arrangement. A *transitional* device - one that also
 * speaks the legacy interface - keeps the old id range at 0x1000 for
 * compatibility and puts its type in the PCI subsystem id instead. QEMU
 * ships several that way by default, `virtio-net-pci` among them, so
 * looking only for the modern id finds the keyboard and not the network
 * card. That was exactly the symptom.
 *
 * Both are the same device and both speak virtio 1.0 to a driver that asks
 * for it; the id range says only what else it is willing to be.
 */
static bool is_kind(const struct pci_device *pci, uint32_t device_id)
{
    if (pci->device >= VIRTIO_PCI_MODERN) {
        return pci->device == VIRTIO_PCI_MODERN + device_id;
    }

    if (pci->device >= VIRTIO_PCI_LEGACY) {
        uint32_t sub = pci_config_read(pci->bus, pci->slot, pci->function,
                                       PCI_SUBSYSTEM);

        return ((sub >> 16) & 0xFFFF) == device_id;
    }

    return false;
}

bool virtio_open(uint32_t device_id, unsigned from_slot,
                 struct virtio_device *dev)
{
    struct pci_device pci;
    unsigned at = 0;
    unsigned matching = 0;

    /*
     * `from_slot` counts devices of *this kind*, which is what the callers
     * mean: `input.c` asks for the first, reads its configuration to see
     * whether it is a keyboard, and asks for the next. So the bus is walked
     * for virtio devices of any kind and only the matching ones are
     * counted.
     */
    for (;;) {
        if (!pci_find(VIRTIO_PCI_VENDOR, PCI_ANY_DEVICE, at, &pci, NULL)) {
            return false;
        }

        at++;

        if (!is_kind(&pci, device_id)) {
            continue;
        }

        if (matching++ >= from_slot) {
            break;
        }
    }

    dev->index = matching - 1;

    pci_enable(&pci);

    dev->base   = find_structure(&pci, VIRTIO_CAP_COMMON, NULL);
    dev->notify = find_structure(&pci, VIRTIO_CAP_NOTIFY, &dev->notify_mul);
    dev->isr    = find_structure(&pci, VIRTIO_CAP_ISR, NULL);
    dev->config = find_structure(&pci, VIRTIO_CAP_DEVICE, NULL);

    if (dev->base == 0 || dev->notify == 0 || dev->isr == 0) {
        return false;           /* not a modern device, whatever it is */
    }

    /*
     * The interrupt line, which is what a driver compares against when
     * `hal_irq_handle` offers it one. **PCI lines are shared**, so unlike
     * the ARM board's window index this does not identify the device on its
     * own - which is why every driver acknowledges through its own ISR and
     * returns when that says nothing happened.
     */
    dev->slot     = pci.irq;
    dev->features = 0;

    return true;
}

/* Reset, then acknowledge, then driver: the same three steps in the same
 * order the other transport takes, at a different address. */
void virtio_begin(const struct virtio_device *dev)
{
    write8(dev->base + COMMON_DEVICE_STATUS, 0);

    while (read8(dev->base + COMMON_DEVICE_STATUS) != 0) {
        /* The spec requires waiting for the reset to be observed. */
    }

    write8(dev->base + COMMON_DEVICE_STATUS, STATUS_ACKNOWLEDGE);
    write8(dev->base + COMMON_DEVICE_STATUS,
           STATUS_ACKNOWLEDGE | STATUS_DRIVER);
}

bool virtio_features(struct virtio_device *dev, uint32_t want)
{
    uint32_t offered;

    mmio_write32(dev->base + COMMON_DEVICE_FEATURE_SEL, 1);

    if ((mmio_read32(dev->base + COMMON_DEVICE_FEATURE)
         & (1u << FEATURE_VERSION_1_BIT)) == 0) {
        virtio_fail(dev);
        return false;
    }

    /* Only what both sides name is agreed, and the caller is told what it
     * got - the argument is `hal/qemu-virt/virtio.c`'s and unchanged. */
    mmio_write32(dev->base + COMMON_DEVICE_FEATURE_SEL, 0);
    offered = mmio_read32(dev->base + COMMON_DEVICE_FEATURE);
    dev->features = offered & want;

    mmio_write32(dev->base + COMMON_DRIVER_FEATURE_SEL, 1);
    mmio_write32(dev->base + COMMON_DRIVER_FEATURE, 1u << FEATURE_VERSION_1_BIT);
    mmio_write32(dev->base + COMMON_DRIVER_FEATURE_SEL, 0);
    mmio_write32(dev->base + COMMON_DRIVER_FEATURE, dev->features);

    write8(dev->base + COMMON_DEVICE_STATUS,
           STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);

    if ((read8(dev->base + COMMON_DEVICE_STATUS) & STATUS_FEATURES_OK) == 0) {
        virtio_fail(dev);
        return false;
    }

    return true;
}

bool virtio_queue_attach(const struct virtio_device *dev, unsigned index,
                         unsigned size, void *desc, void *avail, void *used)
{
    uint16_t max;

    write16(dev->base + COMMON_QUEUE_SELECT, (uint16_t)index);

    max = read16(dev->base + COMMON_QUEUE_SIZE);

    if (max == 0 || max < size) {
        return false;           /* absent, or smaller than we are built for */
    }

    write16(dev->base + COMMON_QUEUE_SIZE, (uint16_t)size);

    /* The kernel is identity mapped, so a pointer into the driver's own
     * storage is already the physical address the device needs - the same
     * sentence the other transport writes, and the same thing that ends the
     * day the kernel moves out of every address space. */
    write64(dev->base + COMMON_QUEUE_DESC,   (uint64_t)(uintptr_t)desc);
    write64(dev->base + COMMON_QUEUE_DRIVER, (uint64_t)(uintptr_t)avail);
    write64(dev->base + COMMON_QUEUE_DEVICE, (uint64_t)(uintptr_t)used);

    virtio_publish();
    write16(dev->base + COMMON_QUEUE_ENABLE, 1);

    return true;
}

void virtio_ready(const struct virtio_device *dev)
{
    write8(dev->base + COMMON_DEVICE_STATUS,
           STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK
           | STATUS_DRIVER_OK);
}

void virtio_fail(const struct virtio_device *dev)
{
    write8(dev->base + COMMON_DEVICE_STATUS, STATUS_FAILED);
}

/*
 * The doorbell, and it is a different address per queue.
 *
 * virtio-mmio has one notify register and the queue number is the *value*
 * written to it. Here the queue number selects the *address*: each queue
 * has an offset the device reports, scaled by the multiplier from the
 * notify capability. A driver that wrote the queue number to one fixed
 * address would ring the wrong bell, or none.
 */
void virtio_notify(const struct virtio_device *dev, unsigned queue)
{
    uint16_t offset;

    write16(dev->base + COMMON_QUEUE_SELECT, (uint16_t)queue);
    offset = read16(dev->base + COMMON_QUEUE_NOTIFY_OFF);

    write16(dev->notify + (uintptr_t)offset * dev->notify_mul,
            (uint16_t)queue);
}

/*
 * Reading the ISR byte is what acknowledges it - there is no separate
 * acknowledge register, because the read clears it. That is the opposite of
 * virtio-mmio, where the status must be written back to the ack register,
 * and writing here would be writing to a device's read-only byte.
 */
uint32_t virtio_ack_interrupt(const struct virtio_device *dev)
{
    return read8(dev->isr);
}

uint32_t virtio_config32(const struct virtio_device *dev, unsigned offset)
{
    return mmio_read32(dev->config + offset);
}

uint8_t virtio_config8(const struct virtio_device *dev, unsigned offset)
{
    return mmio_read8(dev->config + offset);
}

void virtio_config_write8(const struct virtio_device *dev, unsigned offset,
                          uint8_t value)
{
    mmio_write8(dev->config + offset, value);
}

/*
 * On this board an interrupt is a PCI line, already unmasked in the 8259 by
 * nothing at all - so this is where it happens. Several devices may share
 * one, which is why `pc.h` says a driver must decide from its own ISR
 * rather than from the number.
 */
void virtio_enable_interrupt(const struct virtio_device *dev)
{
    pc_irq_unmask(dev->slot);
}
