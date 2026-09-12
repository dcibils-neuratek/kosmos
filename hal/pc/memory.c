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
#include "trampoline.h"


static struct memrange found = { 0, 0 };

/* Every usable byte the loader listed, mappable or not. */
static unsigned long whole;

/*
 * Whether the page a started processor begins on is RAM the loader called
 * usable. Recorded here because the walk is the only moment the map exists:
 * the structure it lives in is free memory to `pmm_init`.
 */
static bool trampoline_free;

/*
 * **The loader's map, whole, and not only the parts this kernel adopts.**
 *
 * `consider` sees type 1 and nothing else, which is right for choosing a
 * region and useless for asking why a machine misbehaves. The pages the
 * userland image sits on are below every region the allocator manages and
 * no process can write them, and on the ThinkPad they change anyway - so
 * the question becomes what *else* the firmware says is down there, and
 * there was no way to ask it. QEMU's map is four entries and clean; a real
 * laptop's is twenty, with ACPI tables, runtime services and reserved holes
 * among them.
 *
 * Copied here for the reason the regions and the command line above are:
 * the structure it lives in is free memory to `pmm_init`, so the walk is
 * the only moment the map exists.
 *
 * **Forty-eight, and the first number was wrong.** Twenty-four was chosen as
 * "more than any machine would have"; OVMF's map is exactly twenty-four, so
 * the very first boot filled the array to the brim and there was no way to
 * tell that from a map that happened to end there. The count seen is
 * reported for that reason - a truncated map has to say so rather than look
 * complete, which is the same mistake as a screenshot check that passes
 * because nothing happened.
 */
#define MEMORY_ENTRIES_MAX 48

static struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} memory_entries[MEMORY_ENTRIES_MAX];

static unsigned memory_entry_count;
static unsigned memory_entries_seen;

static void remember_entry(uint64_t base, uint64_t length, uint32_t type)
{
    memory_entries_seen++;

    if (memory_entry_count >= MEMORY_ENTRIES_MAX) {
        return;
    }

    memory_entries[memory_entry_count].base = base;
    memory_entries[memory_entry_count].length = length;
    memory_entries[memory_entry_count].type = type;
    memory_entry_count++;
}

/* And every usable region that starts below 1 MB, for when it was not. */
#define LOW_REGIONS_MAX 8

static struct {
    unsigned long base;
    unsigned long length;
} low_regions[LOW_REGIONS_MAX];

static unsigned low_region_count;

/*
 * The loader's command line, copied during the walk for the reason the
 * regions above are: the structure it lives in is free memory to `pmm_init`.
 * It is where boot options come from on a machine with no fw_cfg - a laptop
 * booted by GRUB is told `opt/kosmos/smp=1` on the `multiboot2` line or not
 * at all.
 */
static char loader_cmdline[256];

static void keep_cmdline(const char *from, size_t max)
{
    size_t n = 0;

    while (n + 1 < sizeof(loader_cmdline) && n < max && from[n] != '\0') {
        loader_cmdline[n] = from[n];
        n++;
    }

    loader_cmdline[n] = '\0';
}

const char *pc_loader_cmdline(void)
{
    return loader_cmdline;
}

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

bool pc_trampoline_page_free(void)
{
    return trampoline_free;
}

unsigned pc_memory_entries(unsigned *seen)
{
    if (seen != NULL) {
        *seen = memory_entries_seen;
    }

    return memory_entry_count;
}

bool pc_memory_entry(unsigned i, uint64_t *base, uint64_t *length,
                     uint32_t *type)
{
    if (i >= memory_entry_count) {
        return false;
    }

    *base = memory_entries[i].base;
    *length = memory_entries[i].length;
    *type = memory_entries[i].type;
    return true;
}

bool pc_low_region(unsigned i, unsigned long *base, unsigned long *length)
{
    if (i >= low_region_count) {
        return false;
    }

    *base = low_regions[i].base;
    *length = low_regions[i].length;
    return true;
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

/*
 * The disk a loader handed over, as a module. `pc_loader_disk` in
 * `multiboot.h` has why it is kept, and `keep_disk_out_of_ram` below why
 * the region the allocator is given changes because of it.
 */
static struct {
    uint64_t base;
    uint64_t end;
    bool     valid;
} loader_disk;

bool pc_loader_disk(uint64_t *base, uint64_t *bytes)
{
    if (!loader_disk.valid) {
        return false;
    }

    *base = loader_disk.base;
    *bytes = loader_disk.end - loader_disk.base;

    return true;
}

unsigned hal_memory_entries(unsigned *seen)
{
    return pc_memory_entries(seen);
}

bool hal_memory_entry(unsigned i, unsigned long *base, unsigned long *length,
                      unsigned *type)
{
    uint64_t at, len;
    uint32_t kind;

    if (!pc_memory_entry(i, &at, &len, &kind)) {
        return false;
    }

    *base = (unsigned long)at;
    *length = (unsigned long)len;
    *type = (unsigned)kind;
    return true;
}

bool hal_loader_disk(unsigned long *base, unsigned long *bytes)
{
    uint64_t at, length;

    if (!pc_loader_disk(&at, &length)) {
        return false;
    }

    *base = (unsigned long)at;
    *bytes = (unsigned long)length;

    return true;
}

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

    if (base <= TRAMPOLINE_BASE && base + length >= TRAMPOLINE_BASE + 4096u) {
        trampoline_free = true;
    }

    if (base < 0x100000UL && low_region_count < LOW_REGIONS_MAX) {
        low_regions[low_region_count].base = base;
        low_regions[low_region_count].length = length;
        low_region_count++;
    }

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
 * The first module a loader put in memory, remembered as a disk.
 *
 * Only the first: one disk is what `blk_bind.c` binds, and a second module
 * would be a second disk nothing mounts. An empty or backwards range is a
 * loader's mistake and is not believed.
 */
static void remember_disk(uint64_t base, uint64_t end)
{
    if (loader_disk.valid || end <= base) {
        return;
    }

    loader_disk.base = base;
    loader_disk.end = end;
    loader_disk.valid = true;
}

/*
 * **And the pages under it taken out of the allocator's region.**
 *
 * A loader puts a module wherever it likes, and QEMU puts it just past the
 * kernel image - inside the largest usable region, which is the region
 * `pmm_init` hands out a page at a time. The first processes would be built
 * on top of the disk, and the filesystem would mount whatever they wrote.
 *
 * So the region becomes whichever side of the module is larger. The module
 * itself stays where it lies, and `memdisk.c` maps it explicitly either way,
 * since on the side above the new region it is outside the identity map.
 *
 * Whole pages, because the allocator counts in them: the module's first page
 * rounded down and its last rounded up.
 */
static void keep_disk_out_of_ram(void)
{
    unsigned long lo, hi, top, below, above;

    if (!loader_disk.valid || found.size == 0) {
        return;
    }

    lo = (unsigned long)(loader_disk.base & ~(uint64_t)0xfff);
    hi = (unsigned long)((loader_disk.end + 0xfff) & ~(uint64_t)0xfff);
    top = found.base + found.size;

    if (hi <= found.base || lo >= top) {
        return;                         /* not in the region at all */
    }

    below = lo > found.base ? lo - found.base : 0;
    above = top > hi ? top - hi : 0;

    if (above >= below) {
        found.base = hi;
        found.size = above;
    } else {
        found.size = below;
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

    tag = mb2_find(info, MB2_TAG_CMDLINE);

    if (tag != NULL && tag->size > sizeof(*tag)) {
        keep_cmdline((const char *)tag + sizeof(*tag), tag->size - sizeof(*tag));
    }

    tag = mb2_find(info, MB2_TAG_MODULE);

    if (tag != NULL && tag->size >= sizeof(struct mb2_tag_module)) {
        const struct mb2_tag_module *mod = (const struct mb2_tag_module *)tag;

        remember_disk(mod->mod_start, mod->mod_end);
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

                remember_entry(e->base, e->length, e->type);

                if (e->type == 1) {
                    consider((unsigned long)e->base,
                             (unsigned long)e->length);
                }

                at += map->entry_size;
            }
        }
    }

    keep_disk_out_of_ram();
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

    if ((info->flags & MB_FLAG_CMDLINE) != 0 && info->cmdline != 0) {
        keep_cmdline((const char *)(uintptr_t)info->cmdline,
                     sizeof(loader_cmdline));
    }

    if ((info->flags & MB_FLAG_MODS) != 0 && info->mods_count > 0
        && info->mods_addr != 0) {
        const struct multiboot_mod *mod =
            (const struct multiboot_mod *)(uintptr_t)info->mods_addr;

        remember_disk(mod->mod_start, mod->mod_end);
    }

    if ((info->flags & MB_FLAG_MMAP) == 0) {
        return;
    }

    entry = (uintptr_t)info->mmap_addr;
    end   = entry + info->mmap_length;

    while (entry < end) {
        const struct multiboot_mmap *m = (const struct multiboot_mmap *)entry;

        remember_entry(m->base, m->length, m->type);

        if (m->type == 1) {
            consider((unsigned long)m->base, (unsigned long)m->length);
        }

        entry += m->size + 4;       /* `size` does not count itself */
    }

    keep_disk_out_of_ram();
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
