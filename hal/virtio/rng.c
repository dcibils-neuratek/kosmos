/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * virtio-rng: randomness, from whatever the host keeps.
 *
 * **The source HTTPS stands on under QEMU** (`roadmap.md`, the browser:
 * TLS, step 1). Neither QEMU processor here has an instruction for it - the
 * ARM board is a Cortex-A72, ARMv8.0, with no RNDR, and the PC runs QEMU's
 * default model, which offers no RDRAND - so the harness gives both boards
 * this device, and the M700 answers from RDRAND instead (`hal/pc`).
 *
 * The smallest virtio device there is: one queue, no features, no
 * configuration. The driver hands the device a buffer it may write; the
 * device fills some or all of it and says how much in the used ring. So a
 * request is one descriptor, the same shape as `blk.c`'s status byte, and it
 * is synchronous for the same reason `blk.c`'s is: it is asked for a few
 * dozen bytes at a time, from a syscall, and the host answers at once.
 *
 * **Written from the specification** (virtio 1.1, 5.4): device ID 4, queue
 * 0 the request queue. What establishes the rest is a boot that reads bytes
 * that are not the zeros the buffer was cleared to, twice, and differ.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mmu.h"
#include "hal.h"
#include "spinlock.h"
#include "virtio.h"
#include "rng.h"

/* One request at a time; two slots, since the ring's size is a power of
 * two the device may expect to be more than one. */
#define QUEUE_SIZE  2

/* As much as one request asks for: `hal_entropy`'s callers take a few dozen
 * bytes, and a longer ask goes round again. */
#define CHUNK       64u

struct vqueue { VIRTQ_FIELDS(QUEUE_SIZE); };

/* `.bss`, identity mapped, as `blk.c` says of its own. */
static struct {
    struct virtio_device dev;
    bool      present;
    uint16_t  last_used;

    struct vqueue queue;

    _Alignas(16) volatile uint8_t buffer[CHUNK];
} rng;

/* One request in flight at a time, and the structure above says so. */
static struct spinlock rng_lock = SPINLOCK("virtio-rng");

bool virtio_rng_init(void)
{
    unsigned from = 0;

    rng.present = false;

    while (virtio_open(VIRTIO_ID_RNG, from, &rng.dev)) {
        from = rng.dev.index + 1;

        virtio_begin(&rng.dev);

        /* The device has no features of its own; feature 32, the modern
         * layout, is all `virtio_features` asks. */
        if (!virtio_features(&rng.dev, 0)) {
            continue;
        }

        memset(&rng.queue, 0, sizeof(rng.queue));
        rng.last_used = 0;

        if (!virtio_queue_attach(&rng.dev, 0, QUEUE_SIZE, &rng.queue.desc,
                                 &rng.queue.avail, &rng.queue.used)) {
            virtio_fail(&rng.dev);
            continue;
        }

        virtio_ready(&rng.dev);
        rng.present = true;
        return true;
    }

    return false;
}

bool virtio_rng_present(void)
{
    return rng.present;
}

/*
 * Up to `CHUNK` bytes, into the buffer; how many the device wrote, or 0.
 *
 * Bounded by a count and not a clock, as `blk.c`'s is: a device that never
 * answers is no randomness rather than a hung machine.
 */
static size_t request(void)
{
    unsigned long spins;
    uint16_t at;

    memset((void *)rng.buffer, 0, sizeof(rng.buffer));

    rng.queue.desc[0].addr  = (uint64_t)virt_to_phys((const void *)rng.buffer);
    rng.queue.desc[0].len   = CHUNK;
    rng.queue.desc[0].flags = VRING_DESC_F_WRITE;
    rng.queue.desc[0].next  = 0;

    at = rng.queue.avail.idx % QUEUE_SIZE;
    rng.queue.avail.ring[at] = 0;

    virtio_publish();
    rng.queue.avail.idx++;
    virtio_publish();

    virtio_notify(&rng.dev, 0);

    for (spins = 0; spins < 100000000UL; spins++) {
        virtio_consume();

        if (rng.queue.used.idx != rng.last_used) {
            uint32_t len;

            len = rng.queue.used.ring[rng.last_used % QUEUE_SIZE].len;
            rng.last_used = rng.queue.used.idx;
            (void)virtio_ack_interrupt(&rng.dev);
            virtio_consume();

            return (len > CHUNK) ? CHUNK : len;
        }
    }

    return 0;
}

size_t virtio_rng_read(void *buf, size_t bytes)
{
    uint8_t *out = buf;
    size_t done = 0;
    unsigned long flags;

    if (!rng.present || buf == NULL) {
        return 0;
    }

    flags = spin_lock(&rng_lock);

    while (done < bytes) {
        size_t got = request();
        size_t take;

        if (got == 0) {
            break;
        }

        take = (bytes - done < got) ? bytes - done : got;
        memcpy(out + done, (const void *)rng.buffer, take);
        done += take;
    }

    memset((void *)rng.buffer, 0, sizeof(rng.buffer));
    spin_unlock(&rng_lock, flags);

    return done;
}
