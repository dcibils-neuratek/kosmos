/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef HAL_PC_NVME_H
#define HAL_PC_NVME_H

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"

/*
 * The disk on a machine built this decade.
 *
 * Named `nvme_*` rather than `hal_blk_*` for the reason `snd_bind.c` and
 * `input_bind.c` exist: a board may have more than one candidate for the
 * same HAL entry, and which one answers is a decision the *board* makes at
 * run time from what the firmware reports. This one is found on the PCI bus
 * by class; virtio-blk is found in its own way; `blk_bind.c` asks each in
 * turn and owns the `hal_blk_*` names.
 *
 * Everything is synchronous, for the reason `hal.h` gives: reading returns
 * when the bytes are there. See `nvme.c` for why there is no interrupt and
 * what would have to change first.
 */
bool nvme_init(struct blkdev *out);
bool nvme_read(uint64_t sector, void *buf, uint32_t bytes);
bool nvme_write(uint64_t sector, const void *buf, uint32_t bytes);
bool nvme_present(void);

/* What this machine turned out to have, for the boot log's storage line.
 * Says why when there is no disk, which on a laptop is the difference
 * between "no driver" and "a driver that refused this drive". */
const char *nvme_describe(void);

#endif /* HAL_PC_NVME_H */
