/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The ELF reader, on the Mac (`user/init/elfimage.c`, `docs/elf.md` step 2).
 *
 * A Kosmos program built here in memory - two segments, the header at the
 * front of the code - read into the image it describes; then broken one way
 * at a time, each refused with the sentence for that way. And, given the
 * system's own ELF and the flat image `objcopy` made of it, the same bytes:
 * the reader and the build agree on what an image is.
 *
 * Usage: test_elfimage [init.elf init.bin]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/include/elfimage.h"

#define BASE   0x80000000ull
#define MOST   (40ull * 1024 * 1024)
#define PAGE   4096u

static int checks, failed;

static void put(unsigned char *p, uint64_t v, unsigned bytes)
{
    unsigned i;

    for (i = 0; i < bytes; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

/*
 * A program: the ELF header, two program headers, then the code at 0x1000
 * (its first sixteen bytes Kosmos's header, `rx_bytes` one page) and the
 * data at 0x2000, one page each.
 */
#define FILE_LEN (3 * PAGE)

static void build(unsigned char *f)
{
    unsigned char *ph;

    memset(f, 0, FILE_LEN);
    f[0] = 0x7f; f[1] = 'E'; f[2] = 'L'; f[3] = 'F';
    f[4] = 2; f[5] = 1; f[6] = 1;
    put(f + 16, 2, 2);                          /* ET_EXEC */
    put(f + 18, ELFIMAGE_AARCH64, 2);
    put(f + 20, 1, 4);
    put(f + 24, BASE + 16, 8);                  /* the entry */
    put(f + 32, 64, 8);                         /* program headers at 64 */
    put(f + 52, 64, 2);
    put(f + 54, 56, 2);
    put(f + 56, 2, 2);

    ph = f + 64;                                /* the code */
    put(ph, 1, 4);
    put(ph + 4, 5, 4);                          /* read and execute */
    put(ph + 8, PAGE, 8);
    put(ph + 16, BASE, 8);
    put(ph + 32, PAGE, 8);
    put(ph + 40, PAGE, 8);

    ph = f + 64 + 56;                           /* the data */
    put(ph, 1, 4);
    put(ph + 4, 6, 4);                          /* read and write */
    put(ph + 8, 2 * PAGE, 8);
    put(ph + 16, BASE + PAGE, 8);
    put(ph + 32, PAGE, 8);
    put(ph + 40, PAGE, 8);

    put(f + PAGE, 0x534f4d534f4bull, 8);        /* "KOSMOS" */
    put(f + PAGE + 8, PAGE, 8);                 /* rx_bytes */
    memset(f + PAGE + 16, 0xc3, PAGE - 16);     /* the code */
    memset(f + 2 * PAGE, 0x5a, PAGE);           /* the data */
}

static const struct elfimage_want want = { ELFIMAGE_AARCH64, BASE, MOST };

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        failed++;
        printf("  FAIL: %s\n", what);
    }
}

/* Built, changed by `change`, and refused with a sentence containing `says`. */
static void refused(void (*change)(unsigned char *), const char *says, const char *what)
{
    static unsigned char f[FILE_LEN], out[2 * PAGE];
    size_t len = 0;
    const char *why;

    build(f);
    change(f);
    why = elfimage_write(f, FILE_LEN, &want, out, sizeof out, &len);

    if (why == NULL || strstr(why, says) == NULL) {
        printf("  FAIL: %s - said %s\n", what, why ? why : "nothing, and read it");
        failed++;
    }

    checks++;
}

static void bad_magic(unsigned char *f)        { f[1] = 'X'; }
static void thirty_two(unsigned char *f)       { f[4] = 1; }
static void big_endian(unsigned char *f)       { f[5] = 2; }
static void pie(unsigned char *f)              { put(f + 16, 3, 2); }
static void object_file(unsigned char *f)      { put(f + 16, 1, 2); }
static void for_x86(unsigned char *f)          { put(f + 18, ELFIMAGE_X86_64, 2); }
static void headers_past_end(unsigned char *f) { put(f + 32, FILE_LEN - 20, 8); }
static void no_headers(unsigned char *f)       { put(f + 56, 0, 2); }
static void dynamic(unsigned char *f)          { put(f + 64 + 56, 3, 4); }
static void segment_past_end(unsigned char *f) { put(f + 64 + 56 + 32, 0xfffffffffffff000ull, 8);
                                                 put(f + 64 + 56 + 40, 0xfffffffffffff000ull, 8); }
static void file_over_memory(unsigned char *f) { put(f + 64 + 40, 16, 8); }
static void below_base(unsigned char *f)       { put(f + 64 + 16, BASE - PAGE, 8); }
static void too_long(unsigned char *f)         { put(f + 64 + 56 + 40, MOST, 8); }
static void wx(unsigned char *f)               { put(f + 64 + 4, 7, 4); }
static void unreadable(unsigned char *f)       { put(f + 64 + 56 + 4, 2, 4); }
static void overlapping(unsigned char *f)      { put(f + 64 + 56 + 16, BASE + 16, 8); }
static void data_first(unsigned char *f)       { put(f + 64 + 4, 6, 4); }
static void code_after(unsigned char *f)       { put(f + 64 + 56 + 4, 5, 4); }
static void not_kosmos(unsigned char *f)       { put(f + PAGE, 0x1234, 8); }
static void rx_odd(unsigned char *f)           { put(f + PAGE + 8, 100, 8); }
static void rx_short(unsigned char *f)         { put(f + 64 + 40, 2 * PAGE, 8);
                                                 put(f + 64 + 56 + 16, BASE + 2 * PAGE, 8); }
static void data_in_code(unsigned char *f)     { put(f + PAGE + 8, 2 * PAGE, 8); }
static void elsewhere(unsigned char *f)        { put(f + 24, BASE + 64, 8); }

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (fp == NULL) {
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);

    if (buf == NULL || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(buf);
        return NULL;
    }

    fclose(fp);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    static unsigned char f[FILE_LEN], out[2 * PAGE];
    size_t len = 0;
    const char *why;

    /* A good one: two pages, the code and then the data, byte for byte. */
    build(f);
    why = elfimage_size(f, FILE_LEN, &want, &len);
    check(why == NULL && len == 2 * PAGE, "a good program's size");
    why = elfimage_write(f, FILE_LEN, &want, out, sizeof out, &len);
    check(why == NULL && len == 2 * PAGE && memcmp(out, f + PAGE, 2 * PAGE) == 0,
          "a good program's image is its code and its data");
    check(elfimage_write(f, FILE_LEN, &want, out, PAGE, &len) != NULL,
          "too little room for the image is refused");

    refused(bad_magic,        "not an ELF",               "the wrong magic");
    refused(thirty_two,       "64-bit",                   "a 32-bit ELF");
    refused(big_endian,       "little-endian",            "a big-endian ELF");
    refused(pie,              "position-independent",     "a position-independent executable");
    refused(object_file,      "not an executable",        "an object file");
    refused(for_x86,          "x86-64",                   "an ELF for another processor");
    refused(headers_past_end, "past the end",             "program headers past the end");
    refused(no_headers,       "no program headers",       "no program headers");
    refused(dynamic,          "dynamic linker",           "a program asking for a dynamic linker");
    refused(segment_past_end, "past the end of the file", "a segment past the end, sized to wrap");
    refused(file_over_memory, "more bytes in the file",   "a segment larger in the file than in memory");
    refused(below_base,       "below where",              "a segment below the base");
    refused(too_long,         "longer than an image",     "an image longer than 40 MB");
    refused(wx,               "writable and executable",  "a segment both writable and executable");
    refused(unreadable,       "cannot be read",           "a segment that cannot be read");
    refused(overlapping,      "overlapping",              "overlapping segments");
    refused(data_first,       "not code",                 "data first");
    refused(code_after,       "code after its data",      "code after the data");
    refused(not_kosmos,       "not built for Kosmos",     "no Kosmos header");
    refused(rx_odd,           "whole number of pages",    "a code size in the header that is not pages");
    refused(rx_short,         "runs past where",          "code longer than the header says");
    refused(data_in_code,     "inside what its header",   "data inside the code the header describes");
    refused(elsewhere,        "starts somewhere other",   "an entry other than the base plus sixteen");

    /* The system's own image, read the way `objcopy` flattened it. */
    if (argc == 3) {
        size_t elf_len = 0, bin_len = 0, got = 0;
        unsigned char *elf = slurp(argv[1], &elf_len);
        unsigned char *bin = slurp(argv[2], &bin_len);
        unsigned char *img;
        struct elfimage_want sys;

        check(elf != NULL && bin != NULL, "the system's ELF and flat image can be read");

        if (elf != NULL && bin != NULL && elf_len > 20) {
            sys.machine = (unsigned)(elf[18] | (elf[19] << 8));
            sys.base = sys.machine == ELFIMAGE_X86_64 ? 0x40000000ull : BASE;
            sys.most = MOST;
            img = malloc(MOST);
            why = elfimage_write(elf, elf_len, &sys, img, MOST, &got);

            if (why != NULL) {
                printf("  the system's image refused: %s\n", why);
            }

            check(why == NULL && got == bin_len && memcmp(img, bin, got) == 0,
                  "the system's ELF reads into exactly the bytes objcopy made");
            free(img);
        }

        free(elf);
        free(bin);
    }

    if (failed > 0) {
        printf("FAIL: %d of %d checks on the ELF reader\n", failed, checks);
        return 1;
    }

    printf("PASS: %d checks on the ELF reader (a Kosmos program read into its image, "
           "refused %s)\n", checks,
           argc == 3 ? "twenty-three ways, and the system's own ELF read into the "
                       "bytes objcopy made"
                     : "twenty-three ways");
    return 0;
}
