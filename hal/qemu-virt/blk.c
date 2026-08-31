/*
 * virtio-blk: the disk.
 *
 * The device M8 rests on. Everything above it - the format, the journal, the
 * attributes - is a Lua server, and this is the one piece that has to touch
 * hardware, which is why it is here and in C.
 *
 * `roadmap.md` records why it lives in the kernel rather than in a process
 * of its own: a driver in userland is the claim this project makes and the
 * right long-term answer, but it needs MMIO mapped into a process and
 * interrupts delivered to one, and neither exists. Building both here would
 * make the filesystem milestone a milestone about driver infrastructure.
 *
 * The bring-up is the same shape as virtio-input next door - scan the
 * windows, negotiate features, hand over a ring - and the ring structures
 * are deliberately a second copy rather than a shared header. Two devices
 * is not enough to know what the shared interface should look like, which
 * is the argument `hal.md` makes about the HAL itself, and the cost of
 * being wrong about it is a change to a working keyboard.
 *
 * What differs from input, and it is the whole of the file: input hands the
 * device empty buffers and waits to be given events. A block request is a
 * *chain* of three descriptors going both ways - a header the device reads,
 * a data buffer it reads or writes depending on the request, and a status
 * byte it writes - and nothing happens until the driver asks.
 *
 * **Written against the specification from knowledge, not from a copy of
 * `virtio_blk.h`**, which is not on this machine. So the field offsets below
 * are the part to distrust, and the test that writes a sector and reads it
 * back is what actually establishes they are right: every one of them is on
 * the path between those two operations, and a wrong offset cannot produce
 * the bytes that went in.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hal.h"
#include "qemu-virt.h"
#include "mmio.h"

/* virtio_mmio.h, the same registers input.c uses. */
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
#define VIRTIO_ID_BLOCK     2           /* virtio_ids.h */

#define STATUS_ACKNOWLEDGE  1u
#define STATUS_DRIVER       2u
#define STATUS_DRIVER_OK    4u
#define STATUS_FEATURES_OK  8u
#define STATUS_FAILED       0x80u

#define FEATURE_VERSION_1_BIT   0u      /* feature 32, in the high window */

#define VRING_DESC_F_NEXT   1u
#define VRING_DESC_F_WRITE  2u

/* virtio_blk.h */
#define VIRTIO_BLK_T_IN     0u          /* read: the device writes the data */
#define VIRTIO_BLK_T_OUT    1u          /* write: the device reads it */
#define VIRTIO_BLK_S_OK     0u

/*
 * One request at a time, so the ring only has to be big enough for one
 * chain. Three descriptors is what a request is; eight leaves room without
 * pretending there is queuing that does not happen yet.
 *
 * A deeper queue is how a real driver overlaps requests, and it is worth
 * having the moment the filesystem has more than one thread asking. Until
 * then it would be untested capacity.
 */
#define QUEUE_SIZE  8

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vqueue {
    _Alignas(16) struct vring_desc desc[QUEUE_SIZE];

    _Alignas(2) struct {
        uint16_t flags;
        uint16_t idx;
        uint16_t ring[QUEUE_SIZE];
        uint16_t used_event;
    } avail;

    _Alignas(4) struct {
        uint16_t flags;
        uint16_t idx;
        struct {
            uint32_t id;
            uint32_t len;
        } ring[QUEUE_SIZE];
        uint16_t avail_event;
    } used;
};

/*
 * The request header the device reads. Sixteen bytes, little endian, and
 * the sector is counted in units of 512 bytes *whatever* the device reports
 * as its logical block size - that is fixed by the specification and is the
 * detail most likely to be got wrong by assuming otherwise.
 */
struct blk_req_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

/*
 * `.bss`, and load-bearing for the same reason input.c says: the kernel is
 * identity mapped, so a pointer into these structures is already the
 * physical address the device needs. That equality ends the day the kernel
 * moves to TTBR1, and this file is one of the places that will have to know.
 */
static struct {
    uintptr_t base;
    unsigned  slot;
    bool      present;
    uint64_t  sectors;              /* capacity, in 512-byte sectors */
    uint16_t  last_used;

    struct vqueue queue;

    _Alignas(16) struct blk_req_header header;
    _Alignas(16) volatile uint8_t      status;
} blk;

static uint32_t reg_read(unsigned offset)
{
    return mmio_read32(blk.base + offset);
}

static void reg_write(unsigned offset, uint32_t value)
{
    mmio_write32(blk.base + offset, value);
}

/* Everything written must be visible before the index that publishes it. */
static void publish(void)
{
    __asm__ volatile("dmb oshst" ::: "memory");
}

/* And the used ring must be in hand before anything reads what it describes. */
static void consume(void)
{
    __asm__ volatile("dmb oshld" ::: "memory");
}

bool hal_blk_init(struct blkdev *out)
{
    unsigned i;

    blk.present = false;

    for (i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
        unsigned max;

        blk.base = base;

        if (mmio_read32(base + REG_MAGIC) != VIRTIO_MAGIC) {
            continue;
        }

        /* Modern only. QEMU's virtio-mmio defaults to the legacy interface
         * and the Makefile passes -global virtio-mmio.force-legacy=false;
         * reading a legacy device with these structures gives garbage
         * rather than an error, so skipping is the safe answer. */
        if (mmio_read32(base + REG_VERSION) != VIRTIO_VERSION_1) {
            continue;
        }

        if (mmio_read32(base + REG_DEVICE_ID) != VIRTIO_ID_BLOCK) {
            continue;               /* zero means the window is empty */
        }

        reg_write(REG_STATUS, 0);                       /* reset */
        reg_write(REG_STATUS, STATUS_ACKNOWLEDGE);
        reg_write(REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

        /* Feature 32 is bit 0 of the high window, which is what the
         * selector registers are for. Without it the device speaks the
         * legacy layout and every structure here is the wrong shape. */
        reg_write(REG_DEVICE_FEATURES_SEL, 1);

        if ((reg_read(REG_DEVICE_FEATURES)
             & (1u << FEATURE_VERSION_1_BIT)) == 0) {
            reg_write(REG_STATUS, STATUS_FAILED);
            continue;
        }

        /* Nothing else is asked for. Every optional block feature - discard,
         * write zeroes, a topology, multiple queues - is a thing this driver
         * would then have to honour, and none is needed to read a sector. */
        reg_write(REG_DRIVER_FEATURES_SEL, 1);
        reg_write(REG_DRIVER_FEATURES, 1u << FEATURE_VERSION_1_BIT);
        reg_write(REG_DRIVER_FEATURES_SEL, 0);
        reg_write(REG_DRIVER_FEATURES, 0);

        reg_write(REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER
                              | STATUS_FEATURES_OK);

        /* The device may refuse. Re-reading rather than assuming is the
         * whole point of the handshake. */
        if ((reg_read(REG_STATUS) & STATUS_FEATURES_OK) == 0) {
            reg_write(REG_STATUS, STATUS_FAILED);
            continue;
        }

        reg_write(REG_QUEUE_SEL, 0);
        max = reg_read(REG_QUEUE_NUM_MAX);

        if (max == 0 || max < QUEUE_SIZE) {
            reg_write(REG_STATUS, STATUS_FAILED);
            continue;
        }

        memset(&blk.queue, 0, sizeof(blk.queue));
        blk.last_used = 0;

        reg_write(REG_QUEUE_NUM, QUEUE_SIZE);

        reg_write(REG_QUEUE_DESC_LOW,   (uint32_t)(uintptr_t)&blk.queue.desc);
        reg_write(REG_QUEUE_DESC_HIGH,
                  (uint32_t)((uint64_t)(uintptr_t)&blk.queue.desc >> 32));
        reg_write(REG_QUEUE_AVAIL_LOW,  (uint32_t)(uintptr_t)&blk.queue.avail);
        reg_write(REG_QUEUE_AVAIL_HIGH,
                  (uint32_t)((uint64_t)(uintptr_t)&blk.queue.avail >> 32));
        reg_write(REG_QUEUE_USED_LOW,   (uint32_t)(uintptr_t)&blk.queue.used);
        reg_write(REG_QUEUE_USED_HIGH,
                  (uint32_t)((uint64_t)(uintptr_t)&blk.queue.used >> 32));

        publish();
        reg_write(REG_QUEUE_READY, 1);

        reg_write(REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER
                              | STATUS_FEATURES_OK | STATUS_DRIVER_OK);

        /*
         * Capacity: a 64-bit count of 512-byte sectors at offset 0 of the
         * device's configuration space. Read as two 32-bit halves because
         * the window is 32 bits wide.
         */
        blk.sectors = (uint64_t)mmio_read32(base + REG_CONFIG)
                    | ((uint64_t)mmio_read32(base + REG_CONFIG + 4) << 32);

        blk.slot    = i;
        blk.present = true;

        if (out != NULL) {
            out->sectors     = blk.sectors;
            out->sector_size = HAL_BLK_SECTOR;
        }

        return true;
    }

    return false;
}

/*
 * One request, start to finish.
 *
 * Synchronous, and it spins on the used ring rather than sleeping. That is
 * a deliberate first version and it is the thing to fix first when the
 * filesystem starts doing real work: a spin inside a syscall holds the
 * processor while it waits, so a slow device would stall every other thread.
 * Under QEMU a sector arrives in microseconds and the loop turns a handful
 * of times.
 *
 * The bound exists so a device that never answers is a failed read rather
 * than a hung machine. It is a count and not a clock on purpose - this can
 * run before the timer is anything to rely on.
 */
static bool request(uint32_t type, uint64_t sector, void *buf, uint32_t bytes)
{
    unsigned long spins;
    uint16_t at;

    if (!blk.present || buf == NULL || bytes == 0) {
        return false;
    }

    if ((bytes % HAL_BLK_SECTOR) != 0) {
        return false;
    }

    if (sector + bytes / HAL_BLK_SECTOR > blk.sectors) {
        return false;               /* past the end of the disk */
    }

    blk.header.type     = type;
    blk.header.reserved = 0;
    blk.header.sector   = sector;
    blk.status          = 0xff;     /* not a status the device can leave */

    /* The chain. Three descriptors, and which way the middle one points is
     * the only difference between a read and a write. */
    blk.queue.desc[0].addr  = (uint64_t)(uintptr_t)&blk.header;
    blk.queue.desc[0].len   = sizeof(blk.header);
    blk.queue.desc[0].flags = VRING_DESC_F_NEXT;
    blk.queue.desc[0].next  = 1;

    blk.queue.desc[1].addr  = (uint64_t)(uintptr_t)buf;
    blk.queue.desc[1].len   = bytes;
    blk.queue.desc[1].flags = VRING_DESC_F_NEXT
                            | ((type == VIRTIO_BLK_T_IN)
                               ? VRING_DESC_F_WRITE : 0u);
    blk.queue.desc[1].next  = 2;

    blk.queue.desc[2].addr  = (uint64_t)(uintptr_t)&blk.status;
    blk.queue.desc[2].len   = 1;
    blk.queue.desc[2].flags = VRING_DESC_F_WRITE;
    blk.queue.desc[2].next  = 0;

    at = blk.queue.avail.idx % QUEUE_SIZE;
    blk.queue.avail.ring[at] = 0;       /* the head of the chain */

    publish();
    blk.queue.avail.idx++;
    publish();

    reg_write(REG_QUEUE_NOTIFY, 0);

    for (spins = 0; spins < 100000000UL; spins++) {
        consume();

        if (blk.queue.used.idx != blk.last_used) {
            blk.last_used = blk.queue.used.idx;

            /* The interrupt is acknowledged even though nothing waited on
             * it: leaving it asserted would have the controller re-deliver
             * for ever the moment interrupts are enabled for this device. */
            reg_write(REG_INTERRUPT_ACK, reg_read(REG_INTERRUPT_STATUS));

            consume();
            return blk.status == VIRTIO_BLK_S_OK;
        }
    }

    return false;
}

bool hal_blk_read(uint64_t sector, void *buf, uint32_t bytes)
{
    return request(VIRTIO_BLK_T_IN, sector, buf, bytes);
}

bool hal_blk_write(uint64_t sector, const void *buf, uint32_t bytes)
{
    /* The device only reads this one, which the descriptor says by not
     * being marked writable. The cast is losing a const the interface
     * keeps for the caller's sake. */
    return request(VIRTIO_BLK_T_OUT, sector, (void *)(uintptr_t)buf, bytes);
}
