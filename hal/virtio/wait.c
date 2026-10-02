/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Waiting for a virtio device to finish what it was handed (`virtio.h`).
 *
 * **Every driver here waited by a count**: a hundred million turns of a
 * loop, and then a failed request. A count was chosen so the wait could
 * run before the timer was anything to rely on, and the count was sized for
 * a sector arriving in microseconds. Under QEMU on a busy host it does not:
 * on 1 October, with the gate's QEMUs and builds filling this Mac, a read
 * of `/Home` took longer than the loop - about a second under TCG - and the
 * request was given up on while the device still had it (`testing.md`
 * 18.339).
 *
 * **Giving up was the smaller fault.** The driver then kept using the
 * queue, one request behind the device for good: each new request saw the
 * last one's late completion, took it for its own, and returned before the
 * device had begun - a status still at its "not answered" value, so every
 * read failed from then on. Blocks already cached kept answering, so Tracker
 * listed `/Home` and found every file's attributes missing - zero bytes, a
 * folder drawn as a file - and a film opened and closed in a second. The
 * bytes on the disk were never touched; a write in that state could have
 * been.
 *
 * So two things, and both are the point:
 *
 *   - **The clock, not a count**, and long enough for a host that is
 *     merely slow: thirty seconds. The counter runs from reset on both
 *     machines, so this needs no timer; only its rate, which an x86 board
 *     measures a little after boot - until then a rate above any real one
 *     is assumed, which makes the wait longer, never shorter.
 *   - **Done means all of it**: the used index equal to the available one,
 *     not merely moved. And a device that never got there is **reset** and
 *     given up on for good. Not merely left alone: the chain it was handed
 *     points at buffers that are not the driver's to keep - the disk's is
 *     the kernel's bounce buffer, which the next write fills - and a device
 *     still holding them could write a later request's bytes where an
 *     earlier one said. A reset is how virtio hands every buffer back.
 */

#include <stdbool.h>
#include <stdint.h>

#include "console.h"
#include "cpu.h"
#include "virtio.h"

/* An x86 board's counter before its timer has measured it: faster than any
 * real TSC, so a wait computed from it is longer than asked, never shorter. */
#define UNMEASURED_HZ 8000000000ULL

static uint64_t counter_rate(void)
{
    struct cpu_info cpu;

    cpu_identify(&cpu);
    return cpu.counter_hz != 0 ? cpu.counter_hz : UNMEASURED_HZ;
}

bool virtio_wait_done(const volatile uint16_t *used_idx, uint16_t avail_idx)
{
    uint64_t until = cpu_cycles() + (uint64_t)VIRTIO_WAIT_SECONDS * counter_rate();

    for (;;) {
        virtio_consume();

        if (*used_idx == avail_idx) {
            return true;
        }

        if (cpu_cycles() >= until) {
            return false;
        }
    }
}

void virtio_give_up(const struct virtio_device *dev, const char *name)
{
    virtio_reset(dev);

    kputs(name);
    kputs(": no answer in ");
    kputu(VIRTIO_WAIT_SECONDS);
    kputs(" s - reset, and not asked again until the machine restarts\n");
}
