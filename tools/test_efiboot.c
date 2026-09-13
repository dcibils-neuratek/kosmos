/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The UEFI loader's decisions, on this machine, and read back by the kernel.
 *
 * `boot/efi/mbi.c` is the part of the loader that needs no firmware: where a
 * kernel asks to be put, the firmware's memory map in Multiboot 2's shape,
 * and the information structure. Everything it gets wrong shows up on the
 * ThinkPad as a black panel, so it is asked here first - and the structure
 * it builds is read with `hal/pc/loader_fb.c`, the kernel's own parser,
 * because what counts is whether the kernel agrees, not whether this file
 * does.
 *
 *     test_efiboot build/x86_64/kosmos.bin
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbi.h"
#include "multiboot.h"
#include "multiboot2.h"

static int checks;
static int failures;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        failures++;
        printf("FAIL: %s\n", what);
    }
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

/*------------------------------------------------------------------------
 * A header, built the way `boot/x86_64/start.S` builds one.
 *----------------------------------------------------------------------*/

#define IMAGE_BYTES 0x10000u
#define HEADER_AT   0x30u

struct fake {
    uint8_t  bytes[IMAGE_BYTES];
    uint32_t length;            /* where the next tag goes, from HEADER_AT */
};

static void fake_begin(struct fake *f)
{
    memset(f, 0, sizeof(*f));
    f->length = 16;
}

static void fake_tag(struct fake *f, uint16_t type, uint16_t flags,
                     const uint32_t *words, unsigned count)
{
    uint8_t *t = f->bytes + HEADER_AT + f->length;
    uint32_t size = 8 + 4 * count;
    unsigned i;

    put16(t, type);
    put16(t + 2, flags);
    put32(t + 4, size);

    for (i = 0; i < count; i++) {
        put32(t + 8 + 4 * i, words[i]);
    }

    f->length += (size + 7) & ~7u;
}

static void fake_end(struct fake *f, uint32_t arch, int break_checksum)
{
    uint8_t *h = f->bytes + HEADER_AT;
    uint32_t length;

    fake_tag(f, 0, 0, NULL, 0);
    length = f->length;
    put32(h, MB2_HEADER_MAGIC);
    put32(h + 4, arch);
    put32(h + 8, length);
    put32(h + 12, (uint32_t)(-(int64_t)(MB2_HEADER_MAGIC + arch + length))
                  + (break_checksum ? 1u : 0u));
}

/* The usual image: address, entry, a framebuffer request. */
static void fake_usual(struct fake *f, uint32_t entry, uint32_t bss_end)
{
    uint32_t address[] = { 0x100000u + HEADER_AT, 0x100000u, 0, bss_end };
    uint32_t start[] = { entry };
    uint32_t fb[] = { 0, 0, 32 };

    fake_begin(f);
    fake_tag(f, 2, 0, address, 4);
    fake_tag(f, 3, 0, start, 1);
    fake_tag(f, 5, 0, fb, 3);
}

static void test_header(void)
{
    static struct fake f;
    struct mb2_image img;
    const char *why;

    fake_usual(&f, 0x101000u, 0x120000u);
    fake_end(&f, 0, 0);
    why = mb2_image_parse(f.bytes, IMAGE_BYTES, IMAGE_BYTES, &img);
    check(why == NULL, "a well-formed header is refused");

    check(mb2_image_parse(f.bytes, HEADER_AT + 24, IMAGE_BYTES, &img) != NULL,
          "a header longer than the prefix that was read is believed");
    why = mb2_image_parse(f.bytes, IMAGE_BYTES, IMAGE_BYTES, &img);
    check(img.header_offset == HEADER_AT, "the header is found at the wrong offset");
    check(img.load_addr == 0x100000u, "the load address is wrong");
    check(img.load_offset == 0, "the first byte to load is not the file's first");
    check(img.load_bytes == IMAGE_BYTES,
          "load_end_addr 0 does not mean the whole file");
    check(img.load_end == 0x100000u + IMAGE_BYTES, "load_end is wrong");
    check(img.bss_end == 0x120000u, "bss_end is not what the header says");
    check(img.entry == 0x101000u, "the entry point is wrong");
    check(img.wants_framebuffer, "the framebuffer request is not noticed");

    fake_usual(&f, 0x101000u, 0x120000u);
    fake_end(&f, 0, 1);
    check(mb2_image_parse(f.bytes, IMAGE_BYTES, IMAGE_BYTES, &img) != NULL,
          "a header whose checksum does not sum to zero is believed");

    fake_usual(&f, 0x101000u, 0x120000u);
    fake_end(&f, 4, 0);
    check(mb2_image_parse(f.bytes, IMAGE_BYTES, IMAGE_BYTES, &img) != NULL,
          "a MIPS header is accepted by an x86 loader");

    fake_usual(&f, 0x200000u, 0x220000u);
    fake_end(&f, 0, 0);
    check(mb2_image_parse(f.bytes, IMAGE_BYTES, IMAGE_BYTES, &img) != NULL,
          "an entry point past what is loaded is accepted");

    fake_usual(&f, 0x101000u, 0x108000u);
    fake_end(&f, 0, 0);
    check(mb2_image_parse(f.bytes, IMAGE_BYTES, IMAGE_BYTES, &img) != NULL,
          "a bss_end below load_end is accepted");

    {
        uint32_t address[] = { 0x100000u + HEADER_AT, 0x100000u, 0, 0 };

        fake_begin(&f);
        fake_tag(&f, 2, 0, address, 4);
        fake_end(&f, 0, 0);
        check(mb2_image_parse(f.bytes, IMAGE_BYTES, IMAGE_BYTES, &img) != NULL,
              "a header with no entry tag is accepted");
    }

    {
        uint32_t nothing[] = { 0 };

        fake_usual(&f, 0x101000u, 0x120000u);
        fake_tag(&f, 9, 0, nothing, 1);
        fake_end(&f, 0, 0);
        check(mb2_image_parse(f.bytes, IMAGE_BYTES, IMAGE_BYTES, &img) != NULL,
              "a required tag this loader does not know is ignored");

        fake_usual(&f, 0x101000u, 0x120000u);
        fake_tag(&f, 9, 1, nothing, 1);
        fake_end(&f, 0, 0);
        check(mb2_image_parse(f.bytes, IMAGE_BYTES, IMAGE_BYTES, &img) == NULL,
              "an optional tag this loader does not know is refused");
    }

    /* The header moved past 32 KB: the specification's limit. */
    fake_usual(&f, 0x101000u, 0x120000u);
    fake_end(&f, 0, 0);
    memmove(f.bytes + 0x8000u + 8, f.bytes + HEADER_AT, 128);
    memset(f.bytes + HEADER_AT, 0, 128);
    check(mb2_image_parse(f.bytes, IMAGE_BYTES, IMAGE_BYTES, &img) != NULL,
          "a header past the first 32 KB is found");
}

/* The real kernel, as the build made it. */
static void test_kernel(const char *path)
{
    FILE *in = fopen(path, "rb");
    struct mb2_image img;
    uint8_t *bytes;
    long size;
    const char *why;

    check(in != NULL, "the kernel image named on the command line is missing");

    if (in == NULL) {
        return;
    }

    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fseek(in, 0, SEEK_SET);
    bytes = malloc((size_t)size);

    if (bytes == NULL || fread(bytes, 1, (size_t)size, in) != (size_t)size) {
        check(0, "the kernel image could not be read");
        fclose(in);
        free(bytes);
        return;
    }

    fclose(in);
    why = mb2_image_parse(bytes, 36u * 1024, (uint64_t)size, &img);

    if (why != NULL) {
        printf("       %s\n", why);
    }

    check(why == NULL, "the loader refuses the kernel this build made");
    check(img.load_addr == 0x1000000u,
          "the kernel does not ask for the 16 MB `kosmos.ld` puts it at");
    check(img.load_offset == 0,
          "the flat image does not begin at its load address");
    check(img.load_bytes == (uint32_t)size,
          "not all of the file is loaded, or more than the file");
    check(img.bss_end > img.load_end,
          "the kernel reserves nothing past its file, and it has a .bss");
    check(img.entry > img.load_addr && img.entry < img.load_end,
          "the kernel's entry is not inside it");
    check(img.wants_framebuffer, "the kernel's framebuffer request is lost");

    free(bytes);
}

/*------------------------------------------------------------------------
 * The memory map.
 *----------------------------------------------------------------------*/

#define DESCRIPTOR 48u      /* larger than the 40 the specification defines */

static void descriptor(uint8_t *map, unsigned i, uint32_t type,
                       uint64_t base, uint64_t pages)
{
    uint8_t *d = map + i * DESCRIPTOR;

    memset(d, 0xEE, DESCRIPTOR);    /* padding the walk must step over */
    put32(d, type);
    put32(d + 4, 0);
    put64(d + 8, base);
    put64(d + 16, 0);
    put64(d + 24, pages);
    put64(d + 32, 0);
}

static void test_map(void)
{
    uint8_t map[DESCRIPTOR * 11];
    struct mb2_range out[16];
    unsigned n = 0;

    /* A firmware's order is not promised, so these are not in order. */
    descriptor(map, n++, 11, 0xFEC00000u, 1);           /* MMIO */
    descriptor(map, n++, 7,  0x00200000u, 0x700);       /* conventional */
    descriptor(map, n++, 10, 0x00900000u, 0x10);        /* ACPI NVS */
    descriptor(map, n++, 4,  0x00000000u, 1);           /* boot services data */
    descriptor(map, n++, 6,  0x7F000000u, 0x100);       /* runtime data */
    descriptor(map, n++, 2,  0x00100000u, 0x100);       /* loader data */
    descriptor(map, n++, 0,  0x000A0000u, 0x60);        /* reserved */
    descriptor(map, n++, 7,  0x00001000u, 0x9F);        /* conventional */
    descriptor(map, n++, 3,  0x00910000u, 0x10);        /* boot services code */
    descriptor(map, n++, 9,  0x7E000000u, 0x10);        /* ACPI reclaim */
    descriptor(map, n++, 7,  0x40000000u, 0);           /* no pages at all */

    n = efi_map_convert(map, sizeof(map), DESCRIPTOR, out, 16);

    check(n == 8, "the map is not merged into eight ranges");
    check(n >= 1 && out[0].base == 0 && out[0].length == 0xA0000u
          && out[0].type == 1,
          "boot services data and free memory below 640 KB are not one range");
    check(n >= 2 && out[1].base == 0xA0000u && out[1].length == 0x60000u
          && out[1].type == 2, "the legacy hole is not reserved");
    check(n >= 3 && out[2].base == 0x100000u && out[2].length == 0x800000u
          && out[2].type == 1,
          "the loader's own pages and free memory above them are not one "
          "usable range");
    check(n >= 4 && out[3].base == 0x900000u && out[3].type == 4,
          "ACPI NVS is not type 4");
    check(n >= 5 && out[4].base == 0x910000u && out[4].type == 1,
          "memory on either side of NVS is merged across it");
    check(n >= 6 && out[5].base == 0x7E000000u && out[5].type == 3,
          "ACPI reclaimable memory is not type 3");
    check(n >= 7 && out[6].base == 0x7F000000u && out[6].type == 2,
          "runtime services memory is not reserved");
    check(n >= 8 && out[7].base == 0xFEC00000u && out[7].type == 2,
          "a device window is not reserved");

    check(efi_type_to_mb2(14) == 2 && efi_type_to_mb2(0x70000000u) == 2,
          "a type this loader does not know is not reserved");
    check(efi_map_convert(map, sizeof(map), DESCRIPTOR, out, 5) == 6,
          "a map that does not fit is not refused");
    check(efi_map_convert(map, sizeof(map), 24, out, 16) == 0,
          "a descriptor smaller than the specification's is believed");
}

/*------------------------------------------------------------------------
 * The information structure, read by the kernel.
 *----------------------------------------------------------------------*/

static void test_mbi(void)
{
    static uint8_t buf[4096];
    struct mb2_range ranges[3] = {
        { 0x0, 0x9F000, 1 },
        { 0x100000, 0x7FF00000, 1 },
        { 0xFEC00000, 0x1000, 2 },
    };
    uint8_t rsdp[36];
    struct mbi m;
    uint32_t total;
    const struct mb2_info *info = (const struct mb2_info *)buf;
    const struct mb2_tag *tag;
    struct pc_loader_fb fb;
    unsigned i;

    for (i = 0; i < sizeof(rsdp); i++) {
        rsdp[i] = (uint8_t)(0x40 + i);
    }

    mbi_begin(&m, buf, sizeof(buf));
    mbi_cmdline(&m, "opt/kosmos/smp=1");
    mbi_module(&m, 0x2000000u, 0x4000000u, "disk");
    mbi_mmap(&m, ranges, 3);
    mbi_framebuffer(&m, 0x80000000u, 1920 * 4, 1920, 1080, 16, 8, 8, 8, 0, 8);
    mbi_efi64(&m, 0x7FFFF000u);
    mbi_acpi(&m, true, rsdp, sizeof(rsdp));
    total = mbi_end(&m);

    check(total != 0 && total % 8 == 0, "the structure's size is not a whole "
                                        "number of eight-byte steps");
    check(info->total_size == total, "total_size does not say how big it is");

    tag = mb2_find(info, MB2_TAG_CMDLINE);
    check(tag != NULL && strcmp((const char *)tag + 8, "opt/kosmos/smp=1") == 0,
          "the kernel does not read the command line");

    tag = mb2_find(info, MB2_TAG_MODULE);
    check(tag != NULL
          && ((const struct mb2_tag_module *)tag)->mod_start == 0x2000000u
          && ((const struct mb2_tag_module *)tag)->mod_end == 0x4000000u,
          "the kernel does not find the disk where it was put");

    tag = mb2_find(info, MB2_TAG_MMAP);
    check(tag != NULL && ((const struct mb2_tag_mmap *)tag)->entry_size == 24
          && (tag->size - 16) / 24 == 3,
          "the kernel does not read three map entries of 24 bytes");

    if (tag != NULL) {
        const struct mb2_mmap_entry *e =
            (const struct mb2_mmap_entry *)((const uint8_t *)tag + 16);

        check(e[1].base == 0x100000u && e[1].length == 0x7FF00000u
              && e[1].type == 1 && e[2].type == 2,
              "the map entries the kernel reads are not the ones written");
    }

    check(mb2_framebuffer_from(info, &fb) && fb.addr == 0x80000000u
          && fb.pitch == 1920 * 4 && fb.width == 1920 && fb.height == 1080,
          "the kernel refuses the framebuffer, or reads it wrong");

    tag = mb2_find(info, MB2_TAG_EFI64);
    check(tag != NULL && tag->size >= sizeof(struct mb2_tag_efi64)
          && ((const struct mb2_tag_efi64 *)tag)->pointer == 0x7FFFF000u,
          "the kernel does not find the EFI system table, and SMBIOS with it");

    tag = mb2_find(info, MB2_TAG_ACPI_NEW);
    check(tag != NULL && tag->size == 8 + sizeof(rsdp)
          && memcmp((const uint8_t *)tag + 8, rsdp, sizeof(rsdp)) == 0,
          "the kernel does not get the RSDP, and ACPI with it");

    /* Too small a buffer is refused, not written past. */
    memset(buf, 0xAB, sizeof(buf));
    mbi_begin(&m, buf, 64);
    mbi_mmap(&m, ranges, 3);
    check(mbi_end(&m) == 0 && buf[64] == 0xAB,
          "a structure that does not fit is not refused");
}

int main(int argc, char **argv)
{
    test_header();
    test_map();
    test_mbi();

    if (argc > 1) {
        test_kernel(argv[1]);
    }

    if (failures > 0) {
        printf("FAIL: %d of %d checks on the UEFI loader's decisions\n",
               failures, failures + checks);
        return 1;
    }

    printf("PASS: %d checks on the UEFI loader's decisions, read back by the "
           "kernel's own parser.\n", checks);
    return 0;
}
