/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A blob's canary sums, as the kernel will compute them (`roadmap.md` 6zp).
 *
 *   build/host/imagesums FILE
 *
 * prints the whole blob's 64-bit sum, the page size, then each page's 32-bit
 * sum, one number a line in hexadecimal - with `kernel/image_sum.h`, the
 * very functions the kernel checks the userland image with at boot, so the
 * build and the machine cannot disagree about what a sum is.
 *
 * `tools/bin2c.py --incbin` asks this rather than hashing thirty megabytes
 * in Python, which took 3.4 seconds an image; this takes a twentieth of one.
 * The Python hash stays, for the small blobs and for `test_imagesum`'s
 * fixture, which holds the two to each other.
 */

#include <stdio.h>
#include <stdlib.h>

#include "../kernel/image_sum.h"

#define PAGE 4096ul

int main(int argc, char **argv)
{
    FILE *f;
    long size;
    unsigned char *data;
    unsigned long at;

    if (argc != 2) {
        fprintf(stderr, "usage: imagesums FILE\n");
        return 2;
    }

    f = fopen(argv[1], "rb");

    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0
        || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "imagesums: cannot read %s\n", argv[1]);
        return 1;
    }

    data = malloc((size_t)size);

    if (data == NULL || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "imagesums: cannot read all of %s\n", argv[1]);
        return 1;
    }

    fclose(f);

    printf("%016lx\n%lx\n", image_sum64(data, (unsigned long)size), PAGE);

    for (at = 0; at < (unsigned long)size; at += PAGE) {
        unsigned long n = (unsigned long)size - at < PAGE
                          ? (unsigned long)size - at : PAGE;

        printf("%08x\n", image_sum32(data + at, n));
    }

    free(data);
    return 0;
}
