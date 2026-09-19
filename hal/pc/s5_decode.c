/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `\_S5`'s first element, read from its bytes. `s5_decode.h` says why and
 * where the grammar is.
 */

#include "s5_decode.h"

#define NAME_OP     0x08u
#define ROOT_CHAR   0x5Cu       /* '\' */
#define PACKAGE_OP  0x12u
#define ZERO_OP     0x00u
#define ONE_OP      0x01u
#define BYTE_PREFIX 0x0Au
#define WORD_PREFIX 0x0Bu

/* SLP_TYP is bits 12:10 of PM1 control: three bits. */
#define SLP_TYP_MAX 7u

/*
 * Where the package's contents begin, past its PkgLength - or 0 when the
 * encoding runs off the end. ACPI 6.5, 20.2.4: the lead byte's bits 7:6 say
 * how many bytes follow it, 0 to 3; the length itself is not needed, only
 * how much of it there is to skip.
 */
static size_t past_pkg_length(const uint8_t *aml, size_t at, size_t length)
{
    size_t follow;

    if (at >= length) {
        return 0;
    }

    follow = aml[at] >> 6;

    if (at + 1 + follow >= length) {
        return 0;
    }

    return at + 1 + follow;
}

static bool first_element(const uint8_t *aml, size_t at, size_t length,
                          unsigned *out)
{
    unsigned value;

    /* NumElements, one byte, and at least one element after it. */
    if (at + 1 >= length || aml[at] == 0) {
        return false;
    }

    at++;

    switch (aml[at]) {
    case ZERO_OP:
        value = 0;
        break;
    case ONE_OP:
        value = 1;
        break;
    case BYTE_PREFIX:
        if (at + 1 >= length) {
            return false;
        }
        value = aml[at + 1];
        break;
    case WORD_PREFIX:
        if (at + 2 >= length) {
            return false;
        }
        value = aml[at + 1] | ((unsigned)aml[at + 2] << 8);
        break;
    default:
        return false;           /* a method call, a reference: not a number */
    }

    if (value > SLP_TYP_MAX) {
        return false;
    }

    *out = value;
    return true;
}

bool s5_decode(const uint8_t *aml, size_t length, unsigned *slp_typ)
{
    size_t i;

    if (aml == NULL || slp_typ == NULL || length < 7) {
        return false;
    }

    for (i = 0; i + 6 < length; i++) {
        size_t at;

        if (aml[i] != NAME_OP) {
            continue;
        }

        at = i + 1;

        if (aml[at] == ROOT_CHAR) {
            at++;
        }

        if (at + 5 >= length
            || aml[at] != '_' || aml[at + 1] != 'S' || aml[at + 2] != '5'
            || aml[at + 3] != '_' || aml[at + 4] != PACKAGE_OP) {
            continue;
        }

        at = past_pkg_length(aml, at + 5, length);

        return at != 0 && first_element(aml, at, length, slp_typ);
    }

    return false;
}
