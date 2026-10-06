/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `user/kits/tcc/stamp.c` on the Mac: Kosmos's header written into an image
 * TinyCC linked, in place (`docs/tinycc.md`, step C1).
 *
 *   tcc_stamp IMAGE BASE
 */

#include <stdio.h>
#include <stdlib.h>

#include "stamp.h"

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    unsigned char *b;
    const char *why;

    if (argc != 3 || (f = fopen(argv[1], "r+b")) == NULL) {
        fprintf(stderr, "usage: tcc_stamp IMAGE BASE\n");
        return 2;
    }

    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);

    if (n <= 0 || (b = malloc((size_t)n)) == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "tcc_stamp: %s would not read\n", argv[1]);
        return 1;
    }

    why = tcc_stamp(b, (size_t)n, strtoull(argv[2], NULL, 0));

    if (why != NULL) {
        fprintf(stderr, "tcc_stamp: %s: %s\n", argv[1], why);
        return 1;
    }

    rewind(f);
    if (fwrite(b, 1, (size_t)n, f) != (size_t)n || fclose(f) != 0) {
        fprintf(stderr, "tcc_stamp: %s would not write\n", argv[1]);
        return 1;
    }

    return 0;
}
