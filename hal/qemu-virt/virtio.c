/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The virtio-mmio conversation, once.
 *
 * `virtio.h` says why this exists and what it deliberately does not cover.
 * Everything here is sequence: the order of these writes is the protocol,
 * and a step done out of turn is a device that stays silent rather than one
 * that complains.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "qemu-virt.h"
#include "virtio.h"
#include "mmio.h"

static uint32_t reg_read(const struct virtio_device *dev, unsigned offset)
{
    return mmio_read32(dev->base + offset);
}

static void reg_write(const struct virtio_device *dev, unsigned offset,
                      uint32_t value)
{
    mmio_write32(dev->base + offset, value);
}

bool virtio_open(uint32_t device_id, unsigned from_slot,
                 struct virtio_device *dev)
{
    unsigned i;

    for (i = from_slot; i < VIRTIO_MMIO_COUNT; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;

        if (mmio_read32(base + REG_MAGIC) != VIRTIO_MAGIC) {
            continue;
        }

        /*
         * Modern only. QEMU's virtio-mmio defaults to the legacy interface
         * and the Makefile passes `-global virtio-mmio.force-legacy=false`;
         * reading a legacy device with these structures gives garbage rather
         * than an error, so skipping is the safe answer.
         */
        if (mmio_read32(base + REG_VERSION) != VIRTIO_VERSION_1) {
            continue;
        }

        /* Zero means the window is empty. */
        if (mmio_read32(base + REG_DEVICE_ID) != device_id) {
            continue;
        }

        dev->base     = base;
        dev->slot     = i;
        dev->features = 0;

        /* Reset first, because a window may have been half-configured by a
         * driver that looked at it and did not want it. */
        reg_write(dev, REG_STATUS, 0);
        reg_write(dev, REG_STATUS, STATUS_ACKNOWLEDGE);
        reg_write(dev, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

        return true;
    }

    return false;
}

bool virtio_features(struct virtio_device *dev, uint32_t want)
{
    uint32_t offered;

    reg_write(dev, REG_DEVICE_FEATURES_SEL, 1);

    if ((reg_read(dev, REG_DEVICE_FEATURES)
         & (1u << FEATURE_VERSION_1_BIT)) == 0) {
        virtio_fail(dev);
        return false;
    }

    /*
     * The low window is the device-specific half, and only what both sides
     * name is agreed. Asking for a feature the device does not have and then
     * acting as though it does is how a driver reads a field nobody filled
     * in - so what is written back is the intersection, and the caller is
     * told what it got.
     */
    reg_write(dev, REG_DEVICE_FEATURES_SEL, 0);
    offered = reg_read(dev, REG_DEVICE_FEATURES);
    dev->features = offered & want;

    reg_write(dev, REG_DRIVER_FEATURES_SEL, 1);
    reg_write(dev, REG_DRIVER_FEATURES, 1u << FEATURE_VERSION_1_BIT);
    reg_write(dev, REG_DRIVER_FEATURES_SEL, 0);
    reg_write(dev, REG_DRIVER_FEATURES, dev->features);

    reg_write(dev, REG_STATUS,
              STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);

    /* Read back rather than assumed: the device clears the bit to refuse. */
    if ((reg_read(dev, REG_STATUS) & STATUS_FEATURES_OK) == 0) {
        virtio_fail(dev);
        return false;
    }

    return true;
}

bool virtio_queue_attach(const struct virtio_device *dev, unsigned index,
                         unsigned size, void *desc, void *avail, void *used)
{
    uint32_t max;

    reg_write(dev, REG_QUEUE_SEL, index);

    max = reg_read(dev, REG_QUEUE_NUM_MAX);

    if (max == 0 || max < size) {
        return false;           /* absent, or smaller than we are built for */
    }

    reg_write(dev, REG_QUEUE_NUM, size);

    /*
     * The kernel is identity mapped, so a pointer into the driver's own
     * storage is already the physical address the device needs. That
     * equality ends the day the kernel moves to TTBR1, and this is the one
     * place that would have to learn about it.
     */
    reg_write(dev, REG_QUEUE_DESC_LOW,   (uint32_t)(uintptr_t)desc);
    reg_write(dev, REG_QUEUE_DESC_HIGH,
              (uint32_t)((uint64_t)(uintptr_t)desc >> 32));
    reg_write(dev, REG_QUEUE_AVAIL_LOW,  (uint32_t)(uintptr_t)avail);
    reg_write(dev, REG_QUEUE_AVAIL_HIGH,
              (uint32_t)((uint64_t)(uintptr_t)avail >> 32));
    reg_write(dev, REG_QUEUE_USED_LOW,   (uint32_t)(uintptr_t)used);
    reg_write(dev, REG_QUEUE_USED_HIGH,
              (uint32_t)((uint64_t)(uintptr_t)used >> 32));

    virtio_publish();
    reg_write(dev, REG_QUEUE_READY, 1);

    return true;
}

void virtio_ready(const struct virtio_device *dev)
{
    reg_write(dev, REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER
                               | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
}

void virtio_fail(const struct virtio_device *dev)
{
    reg_write(dev, REG_STATUS, STATUS_FAILED);
}

void virtio_notify(const struct virtio_device *dev, unsigned queue)
{
    reg_write(dev, REG_QUEUE_NOTIFY, queue);
}

uint32_t virtio_ack_interrupt(const struct virtio_device *dev)
{
    uint32_t status = reg_read(dev, REG_INTERRUPT_STATUS);

    if (status != 0) {
        reg_write(dev, REG_INTERRUPT_ACK, status);
    }

    return status;
}

uint32_t virtio_config32(const struct virtio_device *dev, unsigned offset)
{
    return mmio_read32(dev->base + REG_CONFIG + offset);
}

uint8_t virtio_config8(const struct virtio_device *dev, unsigned offset)
{
    return mmio_read8(dev->base + REG_CONFIG + offset);
}
