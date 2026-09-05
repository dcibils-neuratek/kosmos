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
 * The bring-up - scan the windows, negotiate features, hand over a ring -
 * is `virtio.c` now. It was a second copy of input's on purpose, on the
 * grounds that two devices is not enough to know what the shared interface
 * should look like; there are four, and that argument expired somewhere
 * around the third.
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
#include "virtio.h"
#include "mmio.h"

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

struct vqueue { VIRTQ_FIELDS(QUEUE_SIZE); };

/* virtio_blk.h */
#define VIRTIO_BLK_T_IN     0u          /* read: the device writes the data */
#define VIRTIO_BLK_T_OUT    1u          /* write: the device reads it */
#define VIRTIO_BLK_S_OK     0u

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
    struct virtio_device dev;
    bool      present;
    uint64_t  sectors;              /* capacity, in 512-byte sectors */
    uint16_t  last_used;

    struct vqueue queue;

    _Alignas(16) struct blk_req_header header;
    _Alignas(16) volatile uint8_t      status;
} blk;

bool hal_blk_init(struct blkdev *out)
{
    unsigned from = 0;

    blk.present = false;

    while (virtio_open(VIRTIO_ID_BLOCK, from, &blk.dev)) {
        from = blk.dev.slot + 1;

        /*
         * Nothing beyond feature 32 is asked for. Every optional block
         * feature - discard, write zeroes, a topology, multiple queues - is
         * a thing this driver would then have to honour, and none is needed
         * to read a sector.
         */
        if (!virtio_features(&blk.dev, 0)) {
            continue;
        }

        memset(&blk.queue, 0, sizeof(blk.queue));
        blk.last_used = 0;

        if (!virtio_queue_attach(&blk.dev, 0, QUEUE_SIZE, &blk.queue.desc,
                                 &blk.queue.avail, &blk.queue.used)) {
            virtio_fail(&blk.dev);
            continue;
        }

        virtio_ready(&blk.dev);

        /*
         * Capacity: a 64-bit count of 512-byte sectors at offset 0 of the
         * device's configuration space. Read as two 32-bit halves because
         * the window is 32 bits wide.
         */
        blk.sectors = (uint64_t)virtio_config32(&blk.dev, 0)
                    | ((uint64_t)virtio_config32(&blk.dev, 4) << 32);

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

    virtio_publish();
    blk.queue.avail.idx++;
    virtio_publish();

    virtio_notify(&blk.dev, 0);

    for (spins = 0; spins < 100000000UL; spins++) {
        virtio_consume();

        if (blk.queue.used.idx != blk.last_used) {
            blk.last_used = blk.queue.used.idx;

            /* The interrupt is acknowledged even though nothing waited on
             * it: leaving it asserted would have the controller re-deliver
             * for ever the moment interrupts are enabled for this device. */
            (void)virtio_ack_interrupt(&blk.dev);

            virtio_consume();
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
