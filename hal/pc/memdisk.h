/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A disk that is memory the loader filled.
 *
 * Kosmos's own loader (`boot/efi/loader.c`) reads `\boot\disk.img` off the
 * stick through the firmware's file protocol, before there is a kernel to
 * drive the USB host controller, and hands it over as a Multiboot 2 module
 * beside the kernel. GRUB did the same before the loader replaced it. Once
 * the kernel runs, `user/drivers/usb/xhci.c` serves the stick itself through
 * `/Devices/blocks`; this is the disk that exists before that. It presents
 * those bytes as the board's block device, so the filesystem mounts them at
 * boot like any other disk. Writes land in memory and are gone at power-off,
 * which is what a disk carried in RAM is.
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
