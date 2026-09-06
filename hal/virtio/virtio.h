/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * virtio, which is a device rather than a board.
 *
 * **This directory is neither `arch/` nor a board, and that is the point.**
 * `hal/qemu-virt/` and `hal/pc/` answer "which peripherals do you have";
 * these four drivers answer "and how does *this* peripheral work", and the
 * answer is the same on both machines because QEMU gives both the same
 * devices. A disk is a disk whether it was found in an MMIO window the
 * device tree described or behind a PCI capability.
 *
 * What differs is the *transport* - how a register is reached and how the
 * device is discovered - and that is the board's, in its own `virtio.c`
 * behind the calls below. Everything above that line is one copy: two
 * thousand lines that would otherwise have been duplicated the day a second
 * board appeared, and diverged the day after.
 *
 * The boundary was found rather than designed. Before the second board
 * these four files named exactly one board-specific thing between them -
 * `gic_enable_spi(VIRTIO_INTID_BASE + slot)` - and closing that one hole
 * was the whole of what it took.
 */
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
#ifndef HAL_VIRTIO_H
#define HAL_VIRTIO_H

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
    /*
     * Where this device's registers are, in whatever terms the board's
     * transport uses.
     *
     * virtio-mmio needs one address: a window whose layout is fixed, with
     * every register at a known offset from it. virtio-pci needs four and a
     * multiplier, because the specification scatters the structures across
     * BARs and points at each with its own capability - so a board fills
     * what its own `virtio.c` reads and leaves the rest zero.
     */
    uintptr_t base;             /* mmio: the window. pci: the common config */
    uintptr_t notify;           /* pci only */
    uintptr_t isr;              /* pci only */
    uintptr_t config;           /* pci only */
    uint32_t  notify_mul;       /* pci only: bytes between queue doorbells */

    /*
     * Which one this is, in whatever terms the board counts them.
     *
     * An MMIO window index on `qemu-virt`, where the device tree lays
     * thirty-two of them out at a fixed stride; the interrupt line on a PC,
     * where a device is found by walking PCI configuration space and
     * several of them may share one. A driver only ever compares it to what
     * `hal_irq_handle` was given, so what it counts is the board's business
     * and not the driver's.
     */
    unsigned  slot;

    /*
     * Where to resume a scan to find the *next* device of this kind.
     *
     * Not the same as `slot`, and conflating them was a bug: a caller
     * looking for a second device did `from = dev.slot + 1`, which is right
     * when a slot is a window index and nonsense when it is a shared PCI
     * interrupt line. `input.c` is the only caller that scans twice - a
     * keyboard and a tablet answer to one device id - and it found one of
     * them and skipped four.
     */
    unsigned  index;

    uint32_t  features;     /* the low feature window, as agreed */
};

/*
 * Route this device's interrupt to this processor.
 *
 * **The one thing a virtio driver needed a board for**, and it was
 * `gic_enable_spi(VIRTIO_INTID_BASE + dev->slot)` written out in three
 * files - which is a GIC, an INTID base and an MMIO slot numbering, none of
 * which a device driver has any business knowing. With it behind this
 * call, `blk.c`, `net.c`, `input.c` and `snd.c` name nothing board-specific
 * at all, which is what lets them be one copy in `hal/virtio/` rather than
 * two thousand lines duplicated per board.
 */
void virtio_enable_interrupt(const struct virtio_device *dev);

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
 * And writing it, which only one device needs.
 *
 * virtio-input's configuration space is a *window*: a driver writes a
 * selector and a sub-selector into it and reads back whatever that names -
 * which is how a keyboard is told from a tablet before either has been
 * claimed. Every other device here has configuration space that only
 * answers.
 *
 * It exists as a call rather than as arithmetic on `base` because that
 * arithmetic is the transport's. `input.c` computed `base + 0x100 + offset`
 * for a long time, which is exactly right for virtio-mmio and reads
 * somebody else's registers on virtio-pci - where device configuration is
 * in whichever BAR a capability pointed at. The symptom was a machine that
 * found a keyboard, decided the tablet was not one, and had no pointer.
 */
void virtio_config_write8(const struct virtio_device *dev, unsigned offset,
                          uint8_t value);

/*
 * What a board calls when one of these devices raises an interrupt.
 *
 * Offered to each in turn rather than looked up, and every one of them
 * returns immediately unless the line is its own - which is what makes a
 * shared PCI line work without a registry. `hal/qemu-virt/gic.c` records
 * why a registry is not worth its indirection here, and that judgement did
 * not change when a second board arrived; it got easier, because on a PC
 * the line genuinely can belong to more than one of them and each has to
 * ask its own ISR anyway.
 */
void input_interrupt(unsigned line);
void snd_interrupt(unsigned line);
void net_interrupt(unsigned line);

/*
 * The two barriers a ring needs, and which way round they go.
 *
 * `publish`: everything written must be visible to the device before the
 * index that publishes it. `consume`: the used ring must be in hand before
 * anything reads what it describes.
 *
 * **On x86-64 both are a compiler barrier and no instruction at all**, and
 * that is the memory model rather than an omission: TSO does not reorder
 * stores with stores or loads with loads, and the device's view of memory
 * is coherent with the processor's. What TSO does not stop is the
 * *compiler*, which will happily hoist the index write above the buffer
 * writes - so the empty asm with a memory clobber is doing the whole job,
 * and the ARM instruction is doing that job plus one the processor needs.
 *
 * The same pair appears in `user/include/tcpring.h` and `audioring.h` for
 * the rings a process shares with a server, with the same reasoning.
 */
#if defined(__x86_64__)
static inline void virtio_publish(void)
{
    __asm__ volatile("" ::: "memory");
}

static inline void virtio_consume(void)
{
    __asm__ volatile("" ::: "memory");
}
#else
static inline void virtio_publish(void)
{
    __asm__ volatile("dmb oshst" ::: "memory");
}

static inline void virtio_consume(void)
{
    __asm__ volatile("dmb oshld" ::: "memory");
}
#endif

#endif /* HAL_VIRTIO_H */
