/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A disk that is memory the loader filled.
 *
 * GRUB reads the USB stick through the firmware, which Kosmos cannot yet do
 * itself - there is no xHCI driver - and loads a disk image from it as a
 * module beside the kernel. This presents those bytes as the board's block
 * device, so the filesystem mounts them at boot like any other disk. Writes
 * land in memory and are gone at power-off, which is what a disk carried in
 * RAM is.
 *
 * `blk_bind.c` asks it first. `pc_loader_disk` in `multiboot.h` is where the
 * bytes come from and why the page allocator never sees them.
 */
#ifndef KOSMOS_HAL_PC_MEMDISK_H
#define KOSMOS_HAL_PC_MEMDISK_H

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"

bool memdisk_init(struct blkdev *out);
bool memdisk_read(uint64_t sector, void *buf, uint32_t bytes);
bool memdisk_write(uint64_t sector, const void *buf, uint32_t bytes);
bool memdisk_present(void);

/* Which disk it is, or why there is none - by name, for the one machine
 * where the answer cannot be read off a serial line. */
const char *memdisk_describe(void);

#endif
