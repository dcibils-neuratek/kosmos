/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Kosmos's header, written into an image TinyCC linked (`stamp.h`). The same
 * C on the Mac, as `tools/tcc_stamp.c` (step C1), and in the C Kit inside
 * Kosmos (C3). Everything read from the file is checked against its length
 * before it is used, and the sums are written so that they cannot wrap -
 * `user/init/elfimage.c`'s discipline, which will read this image next.
 */

#include <string.h>

#include "stamp.h"

#define PT_LOAD      1u
#define PF_X         1u
#define PAGE         4096u
#define MAGIC        0x534f4d534f4bull      /* "KOSMOS" */

static uint64_t le(const unsigned char *p, unsigned n)
{
    uint64_t v = 0;

    while (n-- > 0) {
        v = (v << 8) | p[n];
    }

    return v;
}

static void put(unsigned char *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

const char *tcc_stamp(unsigned char *file, size_t bytes, uint64_t base)
{
    uint64_t phoff, offset, vaddr, filesz, flags, end;
    unsigned phnum, phentsize, i;

    if (bytes < 64 || memcmp(file, "\177ELF", 4) != 0 || file[4] != 2 || file[5] != 1) {
        return "not a 64-bit little-endian ELF";
    }

    phoff = le(file + 32, 8);
    phentsize = (unsigned)le(file + 54, 2);
    phnum = (unsigned)le(file + 56, 2);

    if (phentsize != 56 || phnum == 0 || phoff > bytes || (uint64_t)phnum * 56 > bytes - phoff) {
        return "its program headers are not inside it";
    }

    for (i = 0; i < phnum; i++) {
        const unsigned char *ph = file + phoff + (uint64_t)i * 56;

        if (le(ph, 4) != PT_LOAD) {
            continue;
        }

        flags = le(ph + 4, 4);
        offset = le(ph + 8, 8);
        vaddr = le(ph + 16, 8);
        filesz = le(ph + 32, 8);

        if (vaddr != base || !(flags & PF_X)) {
            return "its first segment is not the code at the base";
        }

        if (offset > bytes || filesz > bytes - offset || filesz < 16) {
            return "its code is not inside it";
        }

        if (le(file + offset, 8) != 0 || le(file + offset + 8, 8) != 0) {
            return "the header's sixteen bytes are not the empty slot head.c leaves";
        }

        end = (filesz + PAGE - 1) & ~(uint64_t)(PAGE - 1);
        put(file + offset, MAGIC);
        put(file + offset + 8, end);
        return NULL;
    }

    return "it has no loadable segment";
}
