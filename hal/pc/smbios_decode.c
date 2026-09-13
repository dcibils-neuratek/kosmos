/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * SMBIOS, decoded. `smbios_decode.h` says what and why.
 *
 * **Every length is checked against what is there before it is used.** The
 * bytes are firmware's, at an address firmware chose, and a table that says
 * a structure is longer than the table is the one input that would walk this
 * off the end of mapped memory at boot stage three.
 *
 * Nothing from the C library, so the host can compile this file exactly as
 * the kernel does.
 */

#include "smbios_decode.h"

static bool sums_to_zero(const uint8_t *at, size_t n)
{
    uint8_t sum = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        sum = (uint8_t)(sum + at[i]);
    }

    return sum == 0;
}

static bool spells(const uint8_t *at, size_t avail, const char *word, size_t n)
{
    size_t i;

    if (avail < n) {
        return false;
    }

    for (i = 0; i < n; i++) {
        if (at[i] != (uint8_t)word[i]) {
            return false;
        }
    }

    return true;
}

/* Little-endian, and assembled a byte at a time rather than cast: the fields
 * sit at odd offsets inside packed structures, so a cast would be an
 * unaligned read. */
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

bool smbios_entry(const uint8_t *at, size_t avail, struct smbios_table *out)
{
    if (at == NULL || out == NULL) {
        return false;
    }

    /*
     * The 3.0 entry point: 24 bytes, one checksum over all of them, and a
     * 64-bit address, which is the reason it exists - a 2.1 table has to be
     * below 4 GB and this one need not be.
     *
     *   00h  "_SM3_"            05h  checksum          06h  length, 18h
     *   07h  major version      08h  minor version     09h  docrev
     *   0Ah  entry point revision                      0Bh  reserved
     *   0Ch  structure table maximum size, DWORD
     *   10h  structure table address, QWORD
     */
    if (spells(at, avail, "_SM3_", 5)) {
        uint8_t length;

        if (avail < 0x18) {
            return false;
        }

        length = at[0x06];

        if (length < 0x18 || length > avail || !sums_to_zero(at, length)) {
            return false;
        }

        out->major   = at[0x07];
        out->minor   = at[0x08];
        out->length  = le32(at + 0x0C);
        out->address = le64(at + 0x10);

        return out->length != 0 && out->address != 0;
    }

    /*
     * The 2.1 entry point: 31 bytes and two checksums, because its second
     * half is the older DMI entry point kept whole inside it.
     *
     *   00h  "_SM_"             04h  checksum, over the entry point length
     *   05h  length, 1Fh        06h  major version     07h  minor version
     *   08h  maximum structure size                    0Ah  revision
     *   10h  "_DMI_"            15h  intermediate checksum, over 0Fh bytes
     *   16h  structure table length, WORD
     *   18h  structure table address, DWORD
     *   1Ch  number of structures                      1Eh  BCD revision
     *
     * **1Eh is accepted as the length too.** Version 2.1 of the
     * specification printed it that way, the later ones say so in a note,
     * and firmware written against 2.1 followed the page it was given.
     */
    if (spells(at, avail, "_SM_", 4)) {
        uint8_t length;

        if (avail < 0x1F) {
            return false;
        }

        length = at[0x05];

        if (length < 0x1E || length > avail || !sums_to_zero(at, length)) {
            return false;
        }

        if (!spells(at + 0x10, avail - 0x10, "_DMI_", 5)
            || !sums_to_zero(at + 0x10, 0x0F)) {
            return false;
        }

        out->major   = at[0x06];
        out->minor   = at[0x07];
        out->length  = le16(at + 0x16);
        out->address = le32(at + 0x18);

        return out->length != 0 && out->address != 0;
    }

    return false;
}

/*
 * The n-th string of a structure's string set, into `out`.
 *
 * The set follows the formatted area: strings numbered from one, each ending
 * in a NUL, and the set ending in one more. Zero means the firmware gave no
 * string, and so does a number past the last one - a field pointing nowhere
 * is a field with nothing in it, not a reason to read on.
 *
 * `end` is one past the set's final NUL, and nothing at or past it is read.
 */
static void string_at(const uint8_t *set, const uint8_t *end, uint8_t n,
                      char *out, size_t size)
{
    const uint8_t *s = set;
    size_t k = 0;

    out[0] = '\0';

    if (n == 0) {
        return;
    }

    while (s < end && *s != 0) {
        if (--n == 0) {
            for (; s < end && *s != 0; s++) {
                if (k + 1 < size) {
                    out[k++] = (*s < 0x20 || *s == 0x7F) ? '?' : (char)*s;
                }
            }

            out[k] = '\0';
            return;
        }

        while (s < end && *s != 0) {
            s++;
        }

        s++;                            /* past the string's own NUL */
    }
}

bool smbios_system(const uint8_t *table, size_t length,
                   struct smbios_system *out)
{
    const uint8_t *at = table;
    const uint8_t *end;

    if (table == NULL || out == NULL) {
        return false;
    }

    end = table + length;

    /*
     * Structure by structure. Each is a four-byte header - type, length of
     * the formatted area, handle - then the formatted area, then its string
     * set. The length covers only the formatted area, so the only way to the
     * next structure is to find the NUL pair that ends this one's strings.
     */
    while (end - at >= 4) {
        uint8_t type      = at[0];
        uint8_t formatted = at[1];
        const uint8_t *set;
        const uint8_t *s;

        if (formatted < 4 || formatted > end - at) {
            return false;
        }

        set = at + formatted;

        for (s = set; end - s >= 2 && !(s[0] == 0 && s[1] == 0); s++) {
        }

        if (end - s < 2) {
            return false;               /* strings with no end, in reach */
        }

        /*
         * System Information. Manufacturer at 04h, product name at 05h,
         * version at 06h, each a string number. A structure too short to
         * hold a field has no string for it; 2.0's type 1 is eight bytes and
         * has all three.
         */
        if (type == 1) {
            string_at(set, s + 1, formatted > 0x04 ? at[0x04] : 0,
                      out->manufacturer, sizeof(out->manufacturer));
            string_at(set, s + 1, formatted > 0x05 ? at[0x05] : 0,
                      out->product, sizeof(out->product));
            string_at(set, s + 1, formatted > 0x06 ? at[0x06] : 0,
                      out->version, sizeof(out->version));
            return true;
        }

        /* End-of-table, type 127: whatever follows is not the table's. */
        if (type == 127) {
            return false;
        }

        at = s + 2;
    }

    return false;
}
