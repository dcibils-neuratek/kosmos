/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where the RAM is, on a PC.
 *
 * `hal/qemu-virt/memory.c` answers this with a constant, because the ARM
 * virt machine puts RAM at 0x40000000 and that is a property of the machine
 * rather than of the boot. A PC cannot: memory below 1 MB is full of the
 * platform's own history, there is a hole under 4 GB for devices whose size
 * depends on what is plugged in, and the only thing that knows the shape is
 * the firmware.
 *
 * So the loader is asked. Multiboot hands over a map of regions with types,
 * and this picks the largest one marked usable - which on any machine this
 * runs on is the main block above 1 MB, and on a machine with a memory hole
 * is the half the kernel was loaded into.
 *
 * **Largest rather than first**, and that is the difference between working
 * and appearing to: the first usable region on a PC is the 640 KB below the
 * BIOS data area, and a kernel that took it would have a heap of 640 KB and
 * no idea why.
 *
 * Multiboot specification 0.6.96, section 3.3.
 */

#include <stdint.h>

#include "hal.h"

#define MB_FLAG_MMAP    (1u << 6)

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} __attribute__((packed));

/*
 * One entry of the map, and the `size` field is the trap in it.
 *
 * `size` does not include itself. Walking the list by `entry + size` steps
 * four bytes short every time and lands in the middle of the next entry,
 * which produces a plausible list of regions that do not exist. The
 * specification says so in one sentence and it is the sentence everybody
 * misses.
 */
struct multiboot_mmap {
    uint32_t size;
    uint64_t base;
    uint64_t length;
    uint32_t type;              /* 1 is usable; everything else is not */
} __attribute__((packed));

static struct memrange found = { 0, 0 };

void hal_ram_from_multiboot(uint32_t at)
{
    const struct multiboot_info *info = (const struct multiboot_info *)(uintptr_t)at;
    uintptr_t entry, end;

    if (at == 0 || (info->flags & MB_FLAG_MMAP) == 0) {
        return;
    }

    entry = (uintptr_t)info->mmap_addr;
    end   = entry + info->mmap_length;

    while (entry < end) {
        const struct multiboot_mmap *m = (const struct multiboot_mmap *)entry;

        if (m->type == 1 && m->length > found.size) {
            found.base = (unsigned long)m->base;
            found.size = (unsigned long)m->length;
        }

        entry += m->size + 4;       /* `size` does not count itself */
    }
}

void hal_ram_range(struct memrange *out)
{
    *out = found;
}
