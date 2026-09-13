/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What Kosmos's UEFI loader decides with no firmware in reach.
 *
 * Three things, each a function of bytes: where a Multiboot 2 kernel asks to
 * be put, the firmware's memory map in the shape such a kernel reads, and the
 * information structure handed to it. They are here rather than in
 * `loader.c` for two reasons. The host can test every line of them, which
 * matters because what a mistake here looks like on the ThinkPad is a black
 * panel on a machine with no serial port. And the kernel's own reader,
 * `hal/pc/loader_fb.c`, can be made to read what this builds, so the test
 * asks the one party whose opinion counts.
 *
 * **Where the layouts come from, said plainly.** The Multiboot 2
 * specification is not in this project's references. Every layout here is
 * the one `hal/pc/multiboot2.h` reads - which GRUB's output satisfied on every
 * boot before this loader existed, and this loader's output satisfies under
 * OVMF: framebuffer, ACPI, the EFI system table, the disk and the map all
 * reach the kernel. The kernel's headers cite the specification's sections
 * 3.1 and 3.6.13; nothing here adds a section number to those. UEFI memory
 * types and the memory descriptor are the UEFI specification's, as gnu-efi's
 * `efidef.h` transcribes them.
 */
#ifndef KOSMOS_BOOT_EFI_MBI_H
#define KOSMOS_BOOT_EFI_MBI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The header's magic, and how far into the file it may begin - the same
 * 32 KB `boot/x86_64/start.S` promises it is inside. */
#define MB2_HEADER_MAGIC    0xE85250D6u
#define MB2_HEADER_SEARCH   32768u

/* What a loader leaves in `eax`: `MB2_LOADER_MAGIC` in multiboot2.h. */
#define MB2_BOOT_MAGIC      0x36d76289u

/*------------------------------------------------------------------------
 * Where the kernel goes.
 *----------------------------------------------------------------------*/

struct mb2_image {
    uint32_t header_offset;     /* where in the file the header begins */
    uint32_t load_addr;         /* the physical address of load_offset */
    uint32_t load_offset;       /* the file offset that goes there */
    uint32_t load_bytes;        /* how many bytes of the file are copied */
    uint32_t load_end;          /* load_addr + load_bytes */
    uint32_t bss_end;           /* the end of everything that must be reserved */
    uint32_t entry;             /* where to begin, in protected mode */
    bool     wants_framebuffer; /* the header asked for a graphics mode */
};

/*
 * The header of a flat Multiboot 2 image, read and checked. NULL when the
 * image can be loaded, and otherwise a sentence saying why not - which the
 * loader prints, since nothing after it will.
 *
 * **Out of a prefix of the file**, with the file's whole size beside it: the
 * loader has to know where the kernel goes, and claim that memory, before it
 * allocates a buffer the kernel's size - which could otherwise be given a
 * piece of that very range.
 */
const char *mb2_image_parse(const uint8_t *prefix, uint64_t prefix_bytes,
                            uint64_t file_bytes, struct mb2_image *out);

/*------------------------------------------------------------------------
 * The memory map.
 *----------------------------------------------------------------------*/

/* UEFI's EFI_MEMORY_DESCRIPTOR: Type, a pad, PhysicalStart, VirtualStart,
 * NumberOfPages, Attribute. The firmware says how big one is, and it may be
 * bigger than this, so a walk steps by what it says. */
#define EFI_DESCRIPTOR_MIN  40u
#define EFI_PAGE_BYTES      4096u

/* Multiboot 2's memory types, as `hal/pc/memory.c` reads them. */
#define MB2_MEM_AVAILABLE   1u
#define MB2_MEM_RESERVED    2u
#define MB2_MEM_ACPI        3u      /* ACPI tables, reclaimable */
#define MB2_MEM_NVS         4u      /* preserve across hibernation */
#define MB2_MEM_BAD         5u

struct mb2_range {
    uint64_t base;
    uint64_t length;
    uint32_t type;
};

/* One UEFI memory type as Multiboot 2 names it. */
uint32_t efi_type_to_mb2(uint32_t efi_type);

/*
 * The firmware's map as sorted, merged ranges. Answers how many were
 * written, or `max + 1` when they did not fit - a map the kernel would read
 * short is refused rather than handed over.
 */
unsigned efi_map_convert(const uint8_t *map, uint64_t map_bytes,
                         uint64_t descriptor_bytes,
                         struct mb2_range *out, unsigned max);

/*------------------------------------------------------------------------
 * The information structure.
 *----------------------------------------------------------------------*/

struct mbi {
    uint8_t *buf;
    uint32_t cap;
    uint32_t used;
    bool     overflow;
};

void mbi_begin(struct mbi *m, uint8_t *buf, uint32_t cap);
void mbi_cmdline(struct mbi *m, const char *text);
void mbi_module(struct mbi *m, uint32_t start, uint32_t end, const char *name);
void mbi_mmap(struct mbi *m, const struct mb2_range *ranges, unsigned count);
void mbi_framebuffer(struct mbi *m, uint64_t addr, uint32_t pitch,
                     uint32_t width, uint32_t height,
                     uint8_t red_at, uint8_t red_bits,
                     uint8_t green_at, uint8_t green_bits,
                     uint8_t blue_at, uint8_t blue_bits);
void mbi_efi64(struct mbi *m, uint64_t system_table);
void mbi_acpi(struct mbi *m, bool v2, const uint8_t *rsdp, uint32_t bytes);

/* The end tag and the total size. The size, or 0 if anything did not fit. */
uint32_t mbi_end(struct mbi *m);

#endif
