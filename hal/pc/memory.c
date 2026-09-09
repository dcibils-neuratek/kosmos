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

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"
#include "pc.h"


static struct memrange found = { 0, 0 };

/*
 * Where the loader left its structure. `start.S` writes it and `pc.h` says
 * why it is a variable rather than an argument.
 */
uint32_t pc_multiboot;

/*
 * The framebuffer the loader set up, copied out while it is still there.
 *
 * **The comment in `pc.h` said this file's lesson was "the same trap for
 * the next field somebody wants", and this is that field.** The multiboot
 * structure sits in RAM just past the kernel image, which `pmm_init`
 * correctly considers free - so anything read from it after the allocator
 * starts is whatever was allocated over it. The memory map is copied out
 * here for that reason and so is this.
 *
 * It matters more than the map did. `hal_fb_init` runs at boot stage six,
 * long after the allocator, and a framebuffer address read then would be
 * plausible and wrong - which on a machine with no serial port means a
 * black screen and no way to ask why.
 */
static struct {
    uint64_t addr;
    uint32_t pitch;
    uint32_t width, height;
    bool     valid;
} loader_fb;

bool pc_loader_framebuffer(uint64_t *addr, uint32_t *pitch,
                           uint32_t *width, uint32_t *height)
{
    if (!loader_fb.valid) {
        return false;
    }

    *addr = loader_fb.addr;
    *pitch = loader_fb.pitch;
    *width = loader_fb.width;
    *height = loader_fb.height;

    return true;
}

void pc_capture_memory(void)
{
    uint32_t at = pc_multiboot;

    const struct multiboot_info *info = (const struct multiboot_info *)(uintptr_t)at;
    uintptr_t entry, end;

    if (at == 0) {
        return;
    }

    /*
     * The framebuffer first, because it is the field this machine cannot
     * report the loss of. Checked here rather than at use: a loader that
     * set the flag and filled in nothing, or answered with a palette, is a
     * loader whose answer must not reach `struct fb` - everything above it
     * treats a pixel as one 32-bit word.
     */
    {
        struct pc_loader_fb got;

        if (pc_framebuffer_from(info, &got)) {
            loader_fb.addr = got.addr;
            loader_fb.pitch = got.pitch;
            loader_fb.width = got.width;
            loader_fb.height = got.height;
            loader_fb.valid = true;
        }
    }

    if ((info->flags & MB_FLAG_MMAP) == 0) {
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
