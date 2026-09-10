/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where this board's disk comes from, and there is only one answer.
 *
 * `virt` describes its devices in a device tree: thirty-two virtio windows
 * at a fixed address and a fixed stride, and nothing else. There is no PCI
 * bus to walk and therefore no NVMe drive to find, so this file is four
 * forwarding calls and no decision.
 *
 * **It exists anyway, and that is the point of it.** The alternative is
 * `virtio/blk.c` defining `hal_blk_init` for both boards, which is what it
 * did - and it worked exactly until a board had a second candidate for the
 * same entry. Then the file that must not know about boards would have had
 * to. The PC's version of this file is where that choice lives; this one
 * says, in as many words, that here there is nothing to choose between.
 *
 * The same arrangement as `snd_bind.c`, which this board also has and which
 * is also a single forward on this side.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"
#include "blk.h"

bool hal_blk_init(struct blkdev *out)
{
    return virtio_blk_init(out);
}

bool hal_blk_read(uint64_t sector, void *buf, uint32_t bytes)
{
    return virtio_blk_read(sector, buf, bytes);
}

bool hal_blk_write(uint64_t sector, const void *buf, uint32_t bytes)
{
    return virtio_blk_write(sector, buf, bytes);
}

bool hal_blk_present(void)
{
    return virtio_blk_present();
}

const char *hal_blk_describe(void)
{
    return virtio_blk_present() ? "virtio-blk" : "no disk";
}
