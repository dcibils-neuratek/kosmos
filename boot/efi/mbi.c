/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What Kosmos's UEFI loader decides with no firmware in reach. `mbi.h` says
 * what each part is for; this file says how.
 *
 * Every multi-byte value is read and written a byte at a time, little-endian,
 * rather than through a struct: the file and the structure are the
 * specification's layout, not this compiler's, and a packed struct would be a
 * promise about alignment the loader's own compiler options could quietly
 * break.
 */

#include "mbi.h"

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
           | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static size_t text_bytes(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0') {
        n++;
    }

    return n + 1;               /* and the terminator, which is part of it */
}

/*------------------------------------------------------------------------
 * Where the kernel goes.
 *----------------------------------------------------------------------*/

/* Header tag types: the three `start.S` writes, and the rest a loader must
 * at least recognise. */
#define TAG_END             0u
#define TAG_INFO_REQUEST    1u
#define TAG_ADDRESS         2u
#define TAG_ENTRY           3u
#define TAG_CONSOLE_FLAGS   4u
#define TAG_FRAMEBUFFER     5u
#define TAG_MODULE_ALIGN    6u

#define TAG_OPTIONAL        1u      /* bit 0 of a header tag's flags */

static const char *parse_tags(const uint8_t *file, uint64_t size,
                              uint64_t at, uint32_t length,
                              struct mb2_image *out)
/* `file` is the prefix; `size` is the whole file's size. */
{
    uint64_t tag = at + 16u;
    uint64_t end = at + length;
    uint32_t header_addr = 0, load_addr = 0, load_end_addr = 0,
             bss_end_addr = 0, entry = 0;
    bool have_address = false, have_entry = false;
    uint64_t load_offset, available;

    out->wants_framebuffer = false;

    while (tag + 8u <= end) {
        uint16_t type = get16(file + tag);
        uint16_t flags = get16(file + tag + 2);
        uint32_t bytes = get32(file + tag + 4);

        if (bytes < 8u || tag + bytes > end) {
            return "a tag in the kernel's Multiboot 2 header has a size "
                   "that does not fit in it";
        }

        if (type == TAG_END) {
            break;
        }

        switch (type) {
        case TAG_ADDRESS:
            if (bytes < 24u) {
                return "the kernel's address tag is too short";
            }

            header_addr = get32(file + tag + 8);
            load_addr = get32(file + tag + 12);
            load_end_addr = get32(file + tag + 16);
            bss_end_addr = get32(file + tag + 20);
            have_address = true;
            break;

        case TAG_ENTRY:
            if (bytes < 12u) {
                return "the kernel's entry tag is too short";
            }

            entry = get32(file + tag + 8);
            have_entry = true;
            break;

        case TAG_FRAMEBUFFER:
            /* The size it asks for is a preference, and the firmware's own
             * mode is the one this loader hands over. */
            out->wants_framebuffer = true;
            break;

        case TAG_INFO_REQUEST:
        case TAG_CONSOLE_FLAGS:
        case TAG_MODULE_ALIGN:
            break;              /* nothing this loader does changes for them */

        default:
            if ((flags & TAG_OPTIONAL) == 0) {
                return "the kernel's Multiboot 2 header asks for something "
                       "this loader does not do";
            }
            break;
        }

        tag += ((uint64_t)bytes + 7u) & ~(uint64_t)7u;
    }

    if (!have_address) {
        return "the kernel's header has no address tag, and this loader reads "
               "only a flat image";
    }

    if (!have_entry) {
        return "the kernel's header names no entry point";
    }

    /* The header is `header_addr - load_addr` bytes after the first byte to
     * load, so that byte is that far before the header in the file. */
    if (header_addr < load_addr || header_addr - load_addr > at) {
        return "the kernel's header puts its own first byte before the file";
    }

    load_offset = at - (header_addr - load_addr);
    available = size - load_offset;

    if (load_end_addr == 0) {
        if (available > 0xFFFFFFFFull - load_addr) {
            return "the kernel is too big to load below 4 GB";
        }

        load_end_addr = load_addr + (uint32_t)available;
    }

    if (load_end_addr <= load_addr || load_end_addr - load_addr > available) {
        return "the kernel's header asks for more bytes than the file has";
    }

    if (bss_end_addr == 0) {
        bss_end_addr = load_end_addr;
    }

    if (bss_end_addr < load_end_addr) {
        return "the kernel's header reserves less than it loads";
    }

    if (entry < load_addr || entry >= load_end_addr) {
        return "the kernel's entry point is not inside what is loaded";
    }

    out->header_offset = (uint32_t)at;
    out->load_addr = load_addr;
    out->load_offset = (uint32_t)load_offset;
    out->load_bytes = load_end_addr - load_addr;
    out->load_end = load_end_addr;
    out->bss_end = bss_end_addr;
    out->entry = entry;
    return NULL;
}

const char *mb2_image_parse(const uint8_t *file, uint64_t prefix_bytes,
                            uint64_t size, struct mb2_image *out)
{
    uint64_t limit = prefix_bytes < size ? prefix_bytes : size;
    uint64_t at;

    if (limit > MB2_HEADER_SEARCH) {
        limit = MB2_HEADER_SEARCH;
    }

    /* On an eight-byte boundary, and all of it inside the first 32 KB, as
     * `start.S` says. Four words that sum to zero - a magic that happens to
     * appear without its checksum is a coincidence, not a header. */
    for (at = 0; at + 16u <= limit; at += 8u) {
        uint32_t magic = get32(file + at);
        uint32_t arch = get32(file + at + 4);
        uint32_t length = get32(file + at + 8);
        uint32_t checksum = get32(file + at + 12);

        if (magic != MB2_HEADER_MAGIC
            || (uint32_t)(magic + arch + length + checksum) != 0) {
            continue;
        }

        if (arch != 0) {
            return "the kernel's Multiboot 2 header is not for i386 "
                   "protected mode";
        }

        if (length < 16u || at + length > size) {
            return "the kernel's Multiboot 2 header runs past the file";
        }

        if (at + length > prefix_bytes) {
            return "the kernel's Multiboot 2 header runs past what was read "
                   "of it";
        }

        return parse_tags(file, size, at, length, out);
    }

    return "no Multiboot 2 header in the kernel's first 32 KB";
}

/*------------------------------------------------------------------------
 * The memory map.
 *----------------------------------------------------------------------*/

/* UEFI memory types, in the order the specification numbers them. */
#define EFI_RESERVED            0u
#define EFI_LOADER_CODE         1u
#define EFI_LOADER_DATA         2u
#define EFI_BOOT_SERVICES_CODE  3u
#define EFI_BOOT_SERVICES_DATA  4u
#define EFI_RUNTIME_CODE        5u
#define EFI_RUNTIME_DATA        6u
#define EFI_CONVENTIONAL        7u
#define EFI_UNUSABLE            8u
#define EFI_ACPI_RECLAIM        9u
#define EFI_ACPI_NVS            10u

uint32_t efi_type_to_mb2(uint32_t efi_type)
{
    switch (efi_type) {
    /*
     * **Boot services memory and the loader's own are usable**, because
     * after ExitBootServices they are nobody's - the same answer GRUB gives,
     * and the one this kernel was written against. That includes the pages
     * the kernel, its disk and this structure sit in; the kernel keeps
     * itself and the disk out of what it allocates, and copies this out
     * before anything could land on it.
     */
    case EFI_LOADER_CODE:
    case EFI_LOADER_DATA:
    case EFI_BOOT_SERVICES_CODE:
    case EFI_BOOT_SERVICES_DATA:
    case EFI_CONVENTIONAL:
        return MB2_MEM_AVAILABLE;

    case EFI_ACPI_RECLAIM:
        return MB2_MEM_ACPI;

    case EFI_ACPI_NVS:
        return MB2_MEM_NVS;

    case EFI_UNUSABLE:
        return MB2_MEM_BAD;

    /* Runtime services, reserved memory, device windows, and every type a
     * later specification adds: kept away from, which is never wrong. */
    default:
        return MB2_MEM_RESERVED;
    }
}

unsigned efi_map_convert(const uint8_t *map, uint64_t map_bytes,
                         uint64_t descriptor_bytes,
                         struct mb2_range *out, unsigned max)
{
    unsigned count = 0, i, kept;
    uint64_t at;

    if (descriptor_bytes < EFI_DESCRIPTOR_MIN) {
        return 0;
    }

    for (at = 0; at + descriptor_bytes <= map_bytes; at += descriptor_bytes) {
        uint32_t type = get32(map + at);
        uint64_t base = get64(map + at + 8);
        uint64_t pages = get64(map + at + 24);
        struct mb2_range next;

        if (pages == 0 || pages > (UINT64_MAX >> 12)) {
            continue;
        }

        next.base = base;
        next.length = pages * EFI_PAGE_BYTES;
        next.type = efi_type_to_mb2(type);

        if (count == max) {
            return max + 1u;
        }

        /*
         * **Sorted as they arrive.** The specification does not promise the
         * firmware's order, and the merge below is only right on a sorted
         * list. A map is a few hundred entries at most, so insertion is
         * enough.
         */
        i = count;

        while (i > 0 && out[i - 1].base > next.base) {
            out[i] = out[i - 1];
            i--;
        }

        out[i] = next;
        count++;
    }

    /*
     * **Merged, which is not tidiness.** A real firmware's map is hundreds of
     * fragments - boot services data interleaved with free memory - and the
     * kernel adopts the *largest single* usable range. Unmerged, the T14's
     * 0x00100000..0x8e36f000 would arrive as pieces and the kernel would run
     * in one of them.
     */
    kept = 0;

    for (i = 0; i < count; i++) {
        if (kept > 0 && out[kept - 1].type == out[i].type
            && out[kept - 1].base + out[kept - 1].length == out[i].base) {
            out[kept - 1].length += out[i].length;
        } else {
            out[kept++] = out[i];
        }
    }

    return kept;
}

/*------------------------------------------------------------------------
 * The information structure.
 *----------------------------------------------------------------------*/

#define INFO_CMDLINE        1u
#define INFO_MODULE         3u
#define INFO_MMAP           6u
#define INFO_FRAMEBUFFER    8u
#define INFO_EFI64          12u
#define INFO_ACPI_OLD       14u
#define INFO_ACPI_NEW       15u

#define MMAP_ENTRY_BYTES    24u

void mbi_begin(struct mbi *m, uint8_t *buf, uint32_t cap)
{
    m->buf = buf;
    m->cap = cap;
    m->used = 8;                /* total_size and reserved, written at the end */
    m->overflow = cap < 8u;
}

/*
 * A tag's room: its eight-byte header written, the rest zeroed up to the next
 * eight-byte boundary, because every tag begins on one and `mb2_find` steps
 * by the size rounded up.
 */
static uint8_t *tag_open(struct mbi *m, uint32_t type, uint32_t bytes)
{
    uint32_t aligned = (bytes + 7u) & ~7u;
    uint32_t i;
    uint8_t *p;

    if (m->overflow || aligned < bytes || aligned > m->cap - m->used) {
        m->overflow = true;
        return NULL;
    }

    p = m->buf + m->used;

    for (i = 0; i < aligned; i++) {
        p[i] = 0;
    }

    put32(p, type);
    put32(p + 4, bytes);
    m->used += aligned;
    return p;
}

void mbi_cmdline(struct mbi *m, const char *text)
{
    size_t n = text_bytes(text);
    uint8_t *p = tag_open(m, INFO_CMDLINE, (uint32_t)(8u + n));
    size_t i;

    if (p != NULL) {
        for (i = 0; i < n; i++) {
            p[8 + i] = (uint8_t)text[i];
        }
    }
}

void mbi_module(struct mbi *m, uint32_t start, uint32_t end, const char *name)
{
    size_t n = text_bytes(name);
    uint8_t *p = tag_open(m, INFO_MODULE, (uint32_t)(16u + n));
    size_t i;

    if (p != NULL) {
        put32(p + 8, start);
        put32(p + 12, end);

        for (i = 0; i < n; i++) {
            p[16 + i] = (uint8_t)name[i];
        }
    }
}

void mbi_mmap(struct mbi *m, const struct mb2_range *ranges, unsigned count)
{
    uint8_t *p;
    unsigned i;

    if ((uint64_t)count * MMAP_ENTRY_BYTES > 0xFFFFFF00u) {
        m->overflow = true;
        return;
    }

    p = tag_open(m, INFO_MMAP, 16u + count * MMAP_ENTRY_BYTES);

    if (p == NULL) {
        return;
    }

    put32(p + 8, MMAP_ENTRY_BYTES);
    put32(p + 12, 0);           /* entry_version */

    for (i = 0; i < count; i++) {
        uint8_t *e = p + 16 + i * MMAP_ENTRY_BYTES;

        put64(e, ranges[i].base);
        put64(e + 8, ranges[i].length);
        put32(e + 16, ranges[i].type);
        put32(e + 20, 0);
    }
}

/*
 * The framebuffer tag, laid out the way GRUB laid it out - a sixteen-bit
 * reserved word after the type - because that is the layout
 * `hal/pc/multiboot2.h` reads and the one every boot of this kernel before
 * this loader was given. Type 1 is direct RGB,
 * followed by where each colour's bits are.
 */
void mbi_framebuffer(struct mbi *m, uint64_t addr, uint32_t pitch,
                     uint32_t width, uint32_t height,
                     uint8_t red_at, uint8_t red_bits,
                     uint8_t green_at, uint8_t green_bits,
                     uint8_t blue_at, uint8_t blue_bits)
{
    uint8_t *p = tag_open(m, INFO_FRAMEBUFFER, 38u);

    if (p == NULL) {
        return;
    }

    put64(p + 8, addr);
    put32(p + 16, pitch);
    put32(p + 20, width);
    put32(p + 24, height);
    p[28] = 32;                 /* bits per pixel */
    p[29] = 1;                  /* direct RGB */
    put16(p + 30, 0);
    p[32] = red_at;
    p[33] = red_bits;
    p[34] = green_at;
    p[35] = green_bits;
    p[36] = blue_at;
    p[37] = blue_bits;
}

void mbi_efi64(struct mbi *m, uint64_t system_table)
{
    uint8_t *p = tag_open(m, INFO_EFI64, 16u);

    if (p != NULL) {
        put64(p + 8, system_table);
    }
}

void mbi_acpi(struct mbi *m, bool v2, const uint8_t *rsdp, uint32_t bytes)
{
    uint8_t *p = tag_open(m, v2 ? INFO_ACPI_NEW : INFO_ACPI_OLD, 8u + bytes);
    uint32_t i;

    if (p != NULL) {
        for (i = 0; i < bytes; i++) {
            p[8 + i] = rsdp[i];
        }
    }
}

uint32_t mbi_end(struct mbi *m)
{
    if (tag_open(m, 0, 8u) == NULL) {
        return 0;
    }

    put32(m->buf, m->used);
    put32(m->buf + 4, 0);
    return m->used;
}
