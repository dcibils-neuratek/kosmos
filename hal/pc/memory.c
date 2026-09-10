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
#include <string.h>

#include "hal.h"
#include "mmu.h"
#include "multiboot2.h"
#include "pc.h"


static struct memrange found = { 0, 0 };

/* Every usable byte the loader listed, mappable or not. */
static unsigned long whole;

/*
 * And the part of it the line above puts out of reach.
 *
 * Kept separately because "the machine has more than the kernel uses" is
 * true on every PC and means nothing: the first usable region is the 640 KB
 * under the BIOS data area, and not choosing it is the scan working. What
 * is worth reporting is memory lost to the *address space* rather than to
 * the choice, and that is only ever what sits past the line.
 */
static unsigned long beyond;

/*
 * The RSDP the loader handed over, if it did.
 *
 * **Copied out for the reason the memory map and the framebuffer are.** The
 * structure it lives in sits in memory `pmm_init` will consider free, and
 * `pc.h` records that as the trap which ate the command line once already.
 * Thirty-six bytes is the whole of an ACPI 2.0 root pointer.
 *
 * Only what is inside it needs to outlive this: the RSDP names an RSDT or
 * an XSDT, and those live in memory the firmware reserved rather than in
 * anything this kernel allocates from.
 */
static uint8_t loader_rsdp[36];
static bool    loader_rsdp_valid;

/*
 * Where the loader said ACPI's root pointer is, or NULL.
 *
 * **This is the entire reason `boot/x86_64/start.S` carries a second
 * header.** A BIOS leaves the pointer somewhere a scan can find it; UEFI
 * passes it to the loader and leaves nothing behind, so a kernel that only
 * scans finds no ACPI on a UEFI machine - no processor count, no ECAM, and
 * no local APIC. Multiboot 1 has no tag for it and Multiboot 2 has two.
 */
const void *pc_loader_rsdp(void)
{
    return loader_rsdp_valid ? (const void *)loader_rsdp : NULL;
}

/*
 * Where the loader left its structure. `start.S` writes it and `pc.h` says
 * why it is a variable rather than an argument.
 */
uint32_t pc_multiboot;

/*
 * And which protocol left it there.
 *
 * The two information structures share nothing but a length at the front,
 * so the magic in `eax` is the only way to tell them apart. `start.S`
 * parks both.
 */
uint32_t pc_multiboot_magic;

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

/*
 * One usable region, weighed against the ones already seen.
 *
 * **Shared by both loader protocols**, which is why it is a function: the
 * two information structures describe memory in different shapes and the
 * *decision* about it is identical, and having written that decision twice
 * once already - the clipping below is the bug that gave a machine with
 * four gigabytes the block above the PCI hole - it belongs in one place.
 */
static void consider(unsigned long base, unsigned long length)
{
unsigned long lo = base;
unsigned long hi = base + length;

whole += length;

        /*
         * **Clipped to what this kernel can map, before it competes to
         * be the largest.**
         *
         * RAM is identity mapped and a process's space begins at
         * `USER_VA_BASE`, so nothing past `DEVICE_WINDOW_BASE` has
         * anywhere to live that is not already somebody's - and a
         * region entirely above the line is not a candidate at all.
         *
         * **Clipping after choosing was the bug, and it is the
         * interesting one.** A PC with four gigabytes or more does not
         * have one block of memory: the PCI hole splits it, so there is
         * a piece below two gigabytes and a larger piece above four -
         * and "the largest usable region" is the one the kernel is not
         * loaded into and cannot reach. `pmm_init` said `the kernel
         * image does not fit in RAM` and it was exactly right.
         *
         * Every machine this is aimed at has that shape. QEMU with the
         * five hundred megabytes the tests use does not, which is why
         * it took booting one with sixteen gigabytes to see it.
         */
        if (hi > DEVICE_WINDOW_BASE) {
            beyond += hi - (lo > DEVICE_WINDOW_BASE
                            ? lo : DEVICE_WINDOW_BASE);
        }

        if (lo < DEVICE_WINDOW_BASE) {
            if (hi > DEVICE_WINDOW_BASE) {
                hi = DEVICE_WINDOW_BASE;
            }

            if (hi - lo > found.size) {
                found.base = lo;
                found.size = hi - lo;
            }
        }
}

/*
 * Everything a Multiboot 2 loader left, which is the same three facts in a
 * different shape - plus the one Multiboot 1 has no way to carry.
 */
static void capture_multiboot2(const struct mb2_info *info)
{
    const struct mb2_tag *tag;

    {
        struct pc_loader_fb got;

        if (mb2_framebuffer_from(info, &got)) {
            loader_fb.addr = got.addr;
            loader_fb.pitch = got.pitch;
            loader_fb.width = got.width;
            loader_fb.height = got.height;
            loader_fb.valid = true;
        }
    }

    /*
     * **The RSDP, which is the whole reason this protocol is here.**
     *
     * Two tags, one per ACPI revision, and the newer is preferred for the
     * reason `acpi.c` prefers an XSDT: the older structure's pointers are
     * 32 bits, so a table above 4 GB cannot be named in it at all.
     */
    tag = mb2_find(info, MB2_TAG_ACPI_NEW);

    if (tag == NULL) {
        tag = mb2_find(info, MB2_TAG_ACPI_OLD);
    }

    if (tag != NULL && tag->size > sizeof(*tag)) {
        size_t bytes = tag->size - sizeof(*tag);

        if (bytes > sizeof(loader_rsdp)) {
            bytes = sizeof(loader_rsdp);
        }

        memcpy(loader_rsdp, (const uint8_t *)tag + sizeof(*tag), bytes);
        loader_rsdp_valid = true;
    }

    tag = mb2_find(info, MB2_TAG_MMAP);

    if (tag != NULL && tag->size >= sizeof(struct mb2_tag_mmap)) {
        const struct mb2_tag_mmap *map = (const struct mb2_tag_mmap *)tag;
        const uint8_t *at = (const uint8_t *)tag + sizeof(*map);
        const uint8_t *end = (const uint8_t *)tag + tag->size;

        /* `entry_size` rather than `sizeof`, because the entries are
         * allowed to grow and a loader newer than this kernel would put
         * the second one somewhere else. */
        if (map->entry_size >= sizeof(struct mb2_mmap_entry)) {
            while (at + map->entry_size <= end) {
                const struct mb2_mmap_entry *e =
                    (const struct mb2_mmap_entry *)at;

                if (e->type == 1) {
                    consider((unsigned long)e->base,
                             (unsigned long)e->length);
                }

                at += map->entry_size;
            }
        }
    }
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
     * Which protocol, from the magic the loader left. Both headers are in
     * the image and both are answered; `multiboot2.h` says why the second
     * one had to exist.
     */
    if (pc_multiboot_magic == MB2_LOADER_MAGIC) {
        capture_multiboot2((const struct mb2_info *)(uintptr_t)at);
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

        if (m->type == 1) {
            consider((unsigned long)m->base, (unsigned long)m->length);
        }

        entry += m->size + 4;       /* `size` does not count itself */
    }
}

void hal_ram_range(struct memrange *out)
{
    *out = found;
}

/*
 * **The cap is a kernel limit rather than a machine one, and saying so is
 * the whole reason this exists.**
 *
 * A ThinkPad has sixteen gigabytes and this kernel can describe the first
 * 768 megabytes of them. Until the scan above clipped, that machine either
 * panicked to a serial port a laptop does not have or faulted before there
 * was a screen to fault on - so the first thing anybody would have seen was
 * nothing at all.
 *
 * It runs on what it can reach now. Kosmos is six and a half megabytes and
 * holds a desktop in five hundred, so a laptop on 766 of its 16384 is a
 * laptop running - and `mmu.h`'s high-half split is what lifts the ceiling
 * for good. A number that is quietly five per cent of the truth is exactly
 * the kind of thing that has to be printed rather than discovered.
 */
bool hal_ram_capped(unsigned long *whole_bytes)
{
    if (beyond == 0) {
        return false;
    }

    *whole_bytes = whole;

    return true;
}
