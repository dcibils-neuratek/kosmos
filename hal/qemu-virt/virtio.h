/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * virtio over virtio-mmio: the part every device on this board shares.
 *
 * **This was three copies until there were four devices.** `blk.c`,
 * `input.c` and `snd.c` each carried the register map, the status
 * handshake, the feature negotiation and the queue registration, and
 * `qemu-virt.h` recorded the reason:
 *
 *     splitting the transport out before there are two would be inventing
 *     an interface against a single caller
 *
 * That was right when it was written and stopped being right two devices
 * later. A network card is the fourth, and four copies of a sequence whose
 * *order* is load-bearing - each write is a promise to the device about what
 * the driver has already done - is four places for one of them to drift.
 *
 * **What is shared is the conversation; what is not is the ring.** The
 * handshake is identical for every device and getting it wrong produces a
 * device that stays silent rather than an error. How a driver *uses* its
 * queues is not shared at all and should not be: the block driver chains
 * three descriptors and waits, input hands the device empty buffers and is
 * given events, sound has four queues with different jobs. A generic ring
 * abstraction over those three would be an interface invented against
 * callers that disagree - which is the mistake this file exists to undo,
 * one layer up.
 *
 * So each driver still declares its own storage, at its own size, and this
 * only registers it.
 */
#ifndef HAL_QEMU_VIRT_VIRTIO_H
#define HAL_QEMU_VIRT_VIRTIO_H

#include <stdbool.h>
#include <stdint.h>

/* virtio_mmio.h. The offsets, and the only copy of them. */
#define REG_MAGIC               0x000
#define REG_VERSION             0x004
#define REG_DEVICE_ID           0x008
#define REG_DEVICE_FEATURES     0x010
#define REG_DEVICE_FEATURES_SEL 0x014
#define REG_DRIVER_FEATURES     0x020
#define REG_DRIVER_FEATURES_SEL 0x024
#define REG_QUEUE_SEL           0x030
#define REG_QUEUE_NUM_MAX       0x034
#define REG_QUEUE_NUM           0x038
#define REG_QUEUE_READY         0x044
#define REG_QUEUE_NOTIFY        0x050
#define REG_INTERRUPT_STATUS    0x060
#define REG_INTERRUPT_ACK       0x064
#define REG_STATUS              0x070
#define REG_QUEUE_DESC_LOW      0x080
#define REG_QUEUE_DESC_HIGH     0x084
#define REG_QUEUE_AVAIL_LOW     0x090
#define REG_QUEUE_AVAIL_HIGH    0x094
#define REG_QUEUE_USED_LOW      0x0a0
#define REG_QUEUE_USED_HIGH     0x0a4
#define REG_CONFIG              0x100

#define VIRTIO_MAGIC        0x74726976u
#define VIRTIO_VERSION_1    2

/* virtio_ids.h, for the devices this board has. */
#define VIRTIO_ID_NET       1
#define VIRTIO_ID_BLOCK     2
#define VIRTIO_ID_INPUT     18
#define VIRTIO_ID_SOUND     25

#define STATUS_ACKNOWLEDGE  1u
#define STATUS_DRIVER       2u
#define STATUS_DRIVER_OK    4u
#define STATUS_FEATURES_OK  8u
#define STATUS_FAILED       0x80u

/*
 * VIRTIO_F_VERSION_1 is feature 32, which is why there are selector
 * registers: features are read and written thirty-two bits at a time, so
 * bit 32 is bit 0 of window 1. Without it the device speaks the legacy
 * layout and every ring structure here is the wrong shape.
 */
#define FEATURE_VERSION_1_BIT   0u

#define VRING_DESC_F_NEXT   1u
#define VRING_DESC_F_WRITE  2u

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

/*
 * The three rings, at whatever size the driver needs.
 *
 * A macro rather than a type because the size is a per-device decision and
 * the ring arrays carry it in their shape: the block driver wants 8, input
 * 16, sound 32. A single shared size would be one of those three imposed on
 * the other two, and the alignment attributes have to travel with the
 * fields, which a pointer-based abstraction would lose.
 *
 *     struct vqueue { VIRTQ_FIELDS(QUEUE_SIZE); };
 */
#define VIRTQ_FIELDS(size)                              \
    _Alignas(16) struct vring_desc desc[size];          \
                                                        \
    _Alignas(2) struct {                                \
        uint16_t flags;                                 \
        uint16_t idx;                                   \
        uint16_t ring[size];                            \
        uint16_t used_event;                            \
    } avail;                                            \
                                                        \
    _Alignas(4) struct {                                \
        uint16_t flags;                                 \
        uint16_t idx;                                   \
        struct vring_used_elem ring[size];              \
        uint16_t avail_event;                           \
    } used

/*
 * A device this driver has claimed. `slot` is the mmio window, which is
 * also how the interrupt is named - see VIRTIO_INTID_BASE in `qemu-virt.h`.
 */
struct virtio_device {
    uintptr_t base;
    unsigned  slot;
    uint32_t  features;     /* the low feature window, as agreed */
};

/*
 * The handshake, in four calls, because it has a middle.
 *
 * It could have been one function and was not, for a reason `input.c`
 * shows: it scans for two devices behind one device id and tells a keyboard
 * from a tablet by reading configuration space - which the specification
 * says may only be read once the driver has acknowledged. So there has to
 * be a place to stand between "I see you" and "these are the features I
 * want", and naming the steps after the promises they make is clearer than
 * one call with a callback in the middle of it.
 */

/*
 * The next window at or after `from_slot` holding this device, taken as far
 * as ACKNOWLEDGE|DRIVER - so configuration space may be read before the
 * caller decides whether it wants this one.
 *
 * False when there is no such device left.
 */
bool virtio_open(uint32_t device_id, unsigned from_slot,
                 struct virtio_device *dev);

/*
 * Agree features. `want` is the low window; feature 32 is always asked for
 * and always required. False when the device refuses, and it is left FAILED
 * so nothing else picks it up half-configured.
 *
 * What was agreed lands in `dev->features`, because asking for a feature and
 * getting it are different things and a driver that assumes is a driver that
 * reads a field the device never filled in.
 */
bool virtio_features(struct virtio_device *dev, uint32_t want);

/*
 * Register one queue's rings. The storage is the caller's, declared with
 * VIRTQ_FIELDS at the caller's size; this writes the six address registers
 * and the size, and publishes them.
 *
 * False when the device's queue is absent or smaller than the driver was
 * built for, which is a refusal rather than something to work around: a ring
 * shorter than the structure describing it is memory the device will index
 * past.
 */
bool virtio_queue_attach(const struct virtio_device *dev, unsigned index,
                         unsigned size, void *desc, void *avail, void *used);

/* Every queue is attached: the driver is ready to be talked to. */
void virtio_ready(const struct virtio_device *dev);

/* This driver does not want this device, or cannot use it. Marked so, so
 * that nothing else finds it in a half-configured state. */
void virtio_fail(const struct virtio_device *dev);

/* Kick a queue. */
void virtio_notify(const struct virtio_device *dev, unsigned queue);

/* Read the interrupt status and acknowledge exactly what was read - not
 * whatever is set by the time the acknowledgement is written, which is a
 * race that loses an interrupt that arrived in between. */
uint32_t virtio_ack_interrupt(const struct virtio_device *dev);

/* Device configuration space. Byte reads are what the specification allows
 * for a field that is not naturally aligned, which a MAC address is not. */
uint32_t virtio_config32(const struct virtio_device *dev, unsigned offset);
uint8_t  virtio_config8(const struct virtio_device *dev, unsigned offset);

/*
 * The two barriers a ring needs, and which way round they go.
 *
 * `publish`: everything written must be visible to the device before the
 * index that publishes it. `consume`: the used ring must be in hand before
 * anything reads what it describes.
 */
static inline void virtio_publish(void)
{
    __asm__ volatile("dmb oshst" ::: "memory");
}

static inline void virtio_consume(void)
{
    __asm__ volatile("dmb oshld" ::: "memory");
}

#endif /* HAL_QEMU_VIRT_VIRTIO_H */
