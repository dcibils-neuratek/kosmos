/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef USER_SERVERS_VMMBLK_H
#define USER_SERVERS_VMMBLK_H

/*
 * **A sandbox's disk** (`docs/linux.md` L1): virtio-blk on virtio-mmio
 * (VIRTIO 1.2 §5.2), backed by a file a disk server holds - a guest's disk
 * in `/Sandboxes`, its own drive, so a guest that fills it fills nothing
 * else. Its blocks are read into a region and written from one, in place
 * (`DISK_IN_PLACE`): the file keeps the blocks it was made with, and the
 * guest's own filesystem keeps its journal, as on any disk.
 */

#include <stdbool.h>
#include <stdint.h>

/* The disk at `path`, through the drive `disk_cap` reaches; false, and
 * nothing offered to the guest, when it is not a file this can use. */
bool vmmblk_start(long disk_cap, const char *path, bool read_only);

bool vmmblk_on(void);
bool vmmblk_mmio(uint64_t off, unsigned size, bool write, uint64_t *value);
bool vmmblk_raised(void);

/* What it has done, for `VMM_OP_STATE`: requests, and bytes each way. */
void vmmblk_counted(uint64_t *reads, uint64_t *writes);

#endif /* USER_SERVERS_VMMBLK_H */
