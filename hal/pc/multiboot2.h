/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The other loader protocol, and the reason this file exists is one tag.
 *
 * **Multiboot 1 boots this kernel under UEFI perfectly well.** That was
 * measured: GRUB, OVMF, a firmware framebuffer, twelve stages, a desktop.
 * What Multiboot 1 cannot do is hand over an **RSDP**, and without one
 * `hal/pc/acpi.c` cannot find ACPI on a UEFI machine at all - it looks in
 * the two places a BIOS leaves the pointer, and UEFI passes it in the EFI
 * Configuration Table and leaves nothing behind.
 *
 * The cost of that is not subtle. The same image on the same four-processor
 * machine reports four processors and drives the local APIC under
 * `-kernel`, and reports one and falls back to a pair of 8259s under GRUB -
 * so on the machine this whole target exists for, none of the ACPI work
 * would ever have run.
 *
 * `boot/x86_64/start.S` carries both headers and the loader picks. A
 * Multiboot 1 loader finds the first and nothing changes; a Multiboot 2
 * loader finds the second and this is how what it left is read.
 *
 * Multiboot specification 2.0, section 3.6 for the tags.
 */
#ifndef KOSMOS_HAL_PC_MULTIBOOT2_H
#define KOSMOS_HAL_PC_MULTIBOOT2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the loader leaves in `eax`, and the only way to tell the two
 * protocols apart: the structures `ebx` points at share nothing but a
 * length at the front. */
#define MB2_LOADER_MAGIC    0x36d76289u

#define MB2_TAG_END         0u
#define MB2_TAG_MMAP        6u
#define MB2_TAG_FRAMEBUFFER 8u
#define MB2_TAG_ACPI_OLD    14u     /* an RSDP as ACPI 1.0 defined it */
#define MB2_TAG_ACPI_NEW    15u     /* ...and as 2.0 and later do */

/*
 * The whole structure is a length, four reserved bytes, and then tags end
 * to end - each with its own type and size, and each starting on an
 * eight-byte boundary whatever its size says.
 */
struct mb2_info {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed));

struct mb2_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct mb2_tag_mmap {
    struct mb2_tag tag;
    uint32_t entry_size;
    uint32_t entry_version;
} __attribute__((packed));

/*
 * **`entry_size` rather than `sizeof`**, and the specification is explicit
 * that a reader must use it: the entries are allowed to grow, and a kernel
 * that strode through them by its own idea of the size would read the
 * second one at the wrong offset on a loader newer than itself.
 */
struct mb2_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;              /* 1 is usable; everything else is not */
    uint32_t reserved;
} __attribute__((packed));

struct mb2_tag_framebuffer {
    struct mb2_tag tag;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;           /* 1 is direct RGB, which is the only one */
    uint16_t reserved;
} __attribute__((packed));

#define MB2_FB_RGB          1u

/*
 * The first tag of a type, or NULL.
 *
 * Bounded by `total_size` and by a tag size that must be at least a
 * header: the structure is memory a loader wrote and a zero size would be
 * a walk that never advances.
 */
const struct mb2_tag *mb2_find(const struct mb2_info *info, uint32_t type);

/*
 * The same decision `pc_framebuffer_from` makes, about the other protocol's
 * structure.
 *
 * Separated from the reading for that function's reason and checked by the
 * same host test: every rejection is a thing a loader has really done, and
 * the failure it prevents is a black panel on a machine with no serial
 * port.
 */
struct pc_loader_fb;

bool mb2_framebuffer_from(const struct mb2_info *info,
                          struct pc_loader_fb *out);

#endif
