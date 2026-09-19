/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `\_S5`'s sleep type, decoded on the host.
 *
 * QEMU's DSDT gives one answer, 0, and it is the one the board wrote as a
 * constant before it read any - so under QEMU a decoder that found nothing
 * and one that found the right thing would power off alike. The ThinkPad's
 * 7 is the case that matters, and each encoding the grammar allows is here,
 * with the ones that must be refused.
 */

#include <stdio.h>
#include <string.h>

#include "../hal/pc/s5_decode.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

static int decodes(const uint8_t *aml, size_t length, unsigned want)
{
    unsigned got = 99;

    return s5_decode(aml, length, &got) && got == want;
}

static int refuses(const uint8_t *aml, size_t length)
{
    unsigned got = 99;

    return !s5_decode(aml, length, &got) && got == 99;
}

int main(void)
{
    /* The T14 Gen 2i's shape: bytes before it, BytePrefix 7 twice. */
    static const uint8_t t14[] = {
        0x14, 0x09, 0x00, 0x08, '_', 'S', '5', '_', 0x12, 0x08, 0x04,
        0x0A, 0x07, 0x0A, 0x07, 0x00, 0x00, 0x14, 0x09,
    };

    /* q35's: ZeroOp four times. */
    static const uint8_t q35[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00,
    };

    /* Named from the root, `\_S5_`. */
    static const uint8_t rooted[] = {
        0x08, 0x5C, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x0A, 0x05, 0x00,
    };

    /* A PkgLength of two bytes, lead byte 0x40 | low nibble. */
    static const uint8_t wide[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x41, 0x00, 0x04, 0x01, 0x00, 0x00,
        0x00,
    };

    /* WordPrefix, 0x0006. */
    static const uint8_t word[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x07, 0x02, 0x0B, 0x06, 0x00, 0x00,
    };

    /* `_S5_` as a string somewhere else, then the real one. */
    static const uint8_t decoy[] = {
        0x0D, '_', 'S', '5', '_', 0x00, 0x08, '_', 'S', '4', '_', 0x12,
        0x06, 0x04, 0x0A, 0x06, 0x00, 0x00, 0x08, '_', 'S', '5', '_', 0x12,
        0x06, 0x04, 0x0A, 0x07, 0x00, 0x00,
    };

    /* Refused: Ones (FFh), a value past three bits, a method's result,
     * an empty package, and a package cut off by the end of the table. */
    static const uint8_t ones[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0xFF, 0x00, 0x00, 0x00,
    };
    static const uint8_t eight[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x0A, 0x08, 0x00, 0x00,
    };
    static const uint8_t call[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 'S', 'L', 'P', 'T',
    };
    static const uint8_t empty[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x02, 0x00, 0x00,
    };
    static const uint8_t cut[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x0A,
    };
    static const uint8_t absent[] = {
        0x08, '_', 'S', '3', '_', 0x12, 0x06, 0x04, 0x0A, 0x05, 0x00, 0x00,
    };

    check(decodes(t14, sizeof(t14), 7),
          "the T14's _S5 did not decode as 7 - the board would write S0 "
          "and halt, as it did");
    check(decodes(q35, sizeof(q35), 0), "q35's _S5 did not decode as 0");
    check(decodes(rooted, sizeof(rooted), 5), "a root-named \\_S5_ was missed");
    check(decodes(wide, sizeof(wide), 1),
          "a two-byte PkgLength was not skipped as two bytes");
    check(decodes(word, sizeof(word), 6), "a WordPrefix element was misread");
    check(decodes(decoy, sizeof(decoy), 7),
          "a string _S5_ or a neighbouring _S4_ was taken for the name");

    check(refuses(ones, sizeof(ones)), "Ones was taken as a sleep type");
    check(refuses(eight, sizeof(eight)),
          "8 was taken as a sleep type - SLP_TYP has three bits");
    check(refuses(call, sizeof(call)),
          "a name reference was read as a number");
    check(refuses(empty, sizeof(empty)), "an empty package gave a number");
    check(refuses(cut, sizeof(cut)),
          "a package cut off by the table's end was read past it");
    check(refuses(absent, sizeof(absent)), "no _S5_ gave a number");
    check(refuses(NULL, 0), "nothing at all gave a number");

    printf("s5 decode: %d checks, %d failed\n", checks + fails, fails);
    return fails != 0;
}
