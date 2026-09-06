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
#include "pc.h"


static struct memrange found = { 0, 0 };

/*
 * Where the loader left its structure. `start.S` writes it and `pc.h` says
 * why it is a variable rather than an argument.
 */
uint32_t pc_multiboot;

void pc_capture_memory(void)
{
    uint32_t at = pc_multiboot;

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
