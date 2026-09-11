/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The board's disk, when the loader brought one.
 *
 * `pc_loader_disk` says where the first module is; this maps it once and
 * answers sectors out of it with a copy.
 *
 * **Mapped rather than read where it lies**, because where a loader puts a
 * module is its own business. QEMU's `-kernel` puts it just past the kernel,
 * inside the identity map; GRUB under UEFI may put it higher, above the
 * region this kernel maps at all. One path for both is the path that gets
 * exercised - so it always goes through `mmu_map_ram`, cached, into the
 * device window.
 *
 * **The window is the limit.** It is 256 MB for every device mapping the
 * machine makes, the framebuffer and the controllers' registers among them,
 * so an image that does not fit is refused by name in `memdisk_describe`
 * rather than turning into a disk that is not there.
 *
 * Set up once, under a lock: `hal_blk_init` is asked by the process that is
 * given the disk and, separately, by `SYS_DISK_INFO` on behalf of anybody
 * who wants its size, and two cores asking at once must not map it twice.
 * Reads and writes take no lock - each is one copy to or from bytes nothing
 * else maps, and the filesystem server is the one process holding the disk.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hal.h"
#include "memdisk.h"
#include "mmu.h"
#include "multiboot.h"
#include "spinlock.h"

static struct spinlock memdisk_lock = SPINLOCK("memdisk");

static struct {
    uint8_t    *bytes;          /* NULL until mapped, then never again */
    uint64_t    sectors;
    bool        tried;
    const char *why;
} disk = { NULL, 0, false, "no disk from the loader" };

bool memdisk_init(struct blkdev *out)
{
    unsigned long flags = spin_lock(&memdisk_lock);
    uint64_t base, size;
    uintptr_t va;

    if (!disk.tried) {
        disk.tried = true;

        if (!pc_loader_disk(&base, &size)) {
            /* Nothing to say beyond the default: most boots have no disk
             * from the loader, and that is not a fault. */
        } else if (size < HAL_BLK_SECTOR) {
            disk.why = "a disk from the loader smaller than one sector";
        } else if ((va = mmu_map_ram((uintptr_t)base, (size_t)size)) == 0) {
            disk.why = "a disk from the loader too large for the device window";
        } else {
            disk.sectors = size / HAL_BLK_SECTOR;
            disk.why = "a disk the loader handed over, in memory";
            disk.bytes = (uint8_t *)va;
        }
    }

    spin_unlock(&memdisk_lock, flags);

    if (disk.bytes == NULL) {
        return false;
    }

    out->sectors = disk.sectors;
    out->sector_size = HAL_BLK_SECTOR;

    return true;
}

/* Whole sectors, starting inside the disk and ending inside it, and none of
 * the arithmetic able to wrap on a hostile count. */
static bool in_range(uint64_t sector, uint32_t bytes)
{
    uint64_t count = bytes / HAL_BLK_SECTOR;

    return disk.bytes != NULL && bytes != 0
        && bytes % HAL_BLK_SECTOR == 0
        && sector < disk.sectors && count <= disk.sectors - sector;
}

bool memdisk_read(uint64_t sector, void *buf, uint32_t bytes)
{
    if (!in_range(sector, bytes)) {
        return false;
    }

    memcpy(buf, disk.bytes + sector * HAL_BLK_SECTOR, bytes);

    return true;
}

bool memdisk_write(uint64_t sector, const void *buf, uint32_t bytes)
{
    if (!in_range(sector, bytes)) {
        return false;
    }

    memcpy(disk.bytes + sector * HAL_BLK_SECTOR, buf, bytes);

    return true;
}

bool memdisk_present(void)
{
    return disk.bytes != NULL;
}

const char *memdisk_describe(void)
{
    return disk.why;
}
