/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef HAL_VIRTIO_BLK_H
#define HAL_VIRTIO_BLK_H

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"

/*
 * virtio-blk, under its own name.
 *
 * It used to define `hal_blk_init` and the rest directly, which was right
 * while it was the only disk any board had. It is not any more: a PC may
 * have an NVMe drive instead, and on the machine this project is aimed at
 * it certainly does. So the board's `blk_bind.c` owns the HAL names and
 * asks each driver in turn - the arrangement `snd.h` and `input.h` already
 * describe, arrived at for the third time.
 *
 * `hal/qemu-virt/` binds this and nothing else, because a device tree with
 * thirty-two virtio windows in it is the whole of what that machine has.
 */
bool virtio_blk_init(struct blkdev *out);
bool virtio_blk_read(uint64_t sector, void *buf, uint32_t bytes);
bool virtio_blk_write(uint64_t sector, const void *buf, uint32_t bytes);
bool virtio_blk_present(void);

#endif /* HAL_VIRTIO_BLK_H */
