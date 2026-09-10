/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where this board's disk comes from: NVMe first, then virtio-blk.
 *
 * **The same shape as `snd_bind.c` and `input_bind.c`, and the same
 * argument a third time.** A ThinkPad has an NVMe drive on its PCI bus and
 * no virtio anything; a machine under QEMU has whichever it was given. So
 * the real device is asked first - `pci_find_class` walks the bus and
 * answers no when there is nothing, which costs a scan and no more - and
 * the emulated one answers when there is no drive to find.
 *
 * That order is deliberate rather than alphabetical. `make x86` gives the
 * machine a virtio disk *and* can be given an NVMe one, and when both are
 * present the interesting device is the one a laptop actually has: putting
 * virtio first would mean the driver that has to work on hardware was the
 * one never exercised, which is exactly how the i8042's auxiliary port went
 * unrun for months.
 *
 * Neither driver knows the other exists. That is the whole point of the
 * `hal_blk_*` names belonging to a board rather than to a driver, and it is
 * why `virtio/blk.c` had to stop defining them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal.h"
#include "blk.h"
#include "nvme.h"

static bool on_nvme;

bool hal_blk_init(struct blkdev *out)
{
    if (nvme_init(out)) {
        on_nvme = true;
        return true;
    }

    return virtio_blk_init(out);
}

bool hal_blk_read(uint64_t sector, void *buf, uint32_t bytes)
{
    return on_nvme ? nvme_read(sector, buf, bytes)
                   : virtio_blk_read(sector, buf, bytes);
}

bool hal_blk_write(uint64_t sector, const void *buf, uint32_t bytes)
{
    return on_nvme ? nvme_write(sector, buf, bytes)
                   : virtio_blk_write(sector, buf, bytes);
}

bool hal_blk_present(void)
{
    return on_nvme ? nvme_present() : virtio_blk_present();
}

/*
 * And which of them it was.
 *
 * When virtio answered, that is the whole story. When nothing did, the
 * sentence worth printing is the NVMe driver's, because it is the one that
 * can distinguish "no controller on the bus" from "a controller that would
 * not reset" from "a drive whose blocks are not 512 bytes" - and on a
 * laptop with no serial port, the difference between those three is the
 * difference between an evening of guessing and a line to read.
 */
const char *hal_blk_describe(void)
{
    if (on_nvme) {
        return nvme_describe();
    }

    return virtio_blk_present() ? "virtio-blk" : nvme_describe();
}
