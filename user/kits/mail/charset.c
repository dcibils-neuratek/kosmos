/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A text part's characters, made UTF-8 (`mime.h`'s `mime_to_utf8`).
 *
 * **The tables are libparserutils'**, the browser's own, read as they ship
 * in `runtime/upstream/netsurf` - ISO-8859-1 to -16 from 0xA0, Windows' code
 * pages 1250 to 1258 from 0x80, each byte's code point, 0xFFFF for none -
 * rather than a second copy of them here (`docs/mail.md`, "moved, from the
 * browser's build"). Only the tables: what turns a byte into UTF-8 is a
 * dozen lines, and the library's codec machinery would be more than the
 * work it does.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mime.h"

#include "8859_tables.h"
#include "ext8_tables.h"

/* "ISO-8859-15" to "iso-8859-15", and "latin1" and the rest to the name
 * this knows them by. */
static void lower(const char *in, char *out, size_t room)
{
    size_t i = 0;

    for (; in[i] && i + 1 < room; i++) {
        char c = in[i];

        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }

    out[i] = '\0';
}

/* The table for a single-byte charset, and where it starts; NULL for none. */
static const uint32_t *table_of(const char *name, unsigned *first)
{
    static const struct { const char *name; const uint32_t *t; unsigned first; } known[] = {
        { "iso-8859-1", t1, 0xA0 }, { "latin1", t1, 0xA0 }, { "iso8859-1", t1, 0xA0 },
        { "iso-8859-2", t2, 0xA0 }, { "iso-8859-3", t3, 0xA0 }, { "iso-8859-4", t4, 0xA0 },
        { "iso-8859-5", t5, 0xA0 }, { "iso-8859-6", t6, 0xA0 }, { "iso-8859-7", t7, 0xA0 },
        { "iso-8859-8", t8, 0xA0 }, { "iso-8859-9", t9, 0xA0 }, { "iso-8859-10", t10, 0xA0 },
        { "iso-8859-11", t11, 0xA0 }, { "iso-8859-13", t13, 0xA0 }, { "iso-8859-14", t14, 0xA0 },
        { "iso-8859-15", t15, 0xA0 }, { "latin-9", t15, 0xA0 }, { "iso-8859-16", t16, 0xA0 },
        { "windows-1250", w1250, 0x80 }, { "windows-1251", w1251, 0x80 },
        { "windows-1252", w1252, 0x80 }, { "cp1252", w1252, 0x80 },
        { "windows-1253", w1253, 0x80 }, { "windows-1254", w1254, 0x80 },
        { "windows-1255", w1255, 0x80 }, { "windows-1256", w1256, 0x80 },
        { "windows-1257", w1257, 0x80 }, { "windows-1258", w1258, 0x80 },
    };

    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
        if (strcmp(name, known[i].name) == 0) {
            *first = known[i].first;
            return known[i].t;
        }
    }

    return NULL;
}

/* One code point as UTF-8, if it fits; how many bytes it takes either way. */
static size_t put_utf8(uint32_t c, uint8_t *out, size_t at, size_t room)
{
    uint8_t b[4];
    size_t n;

    if (c < 0x80) {
        b[0] = (uint8_t)c, n = 1;
    } else if (c < 0x800) {
        b[0] = (uint8_t)(0xC0 | c >> 6), b[1] = (uint8_t)(0x80 | (c & 63)), n = 2;
    } else if (c < 0x10000) {
        b[0] = (uint8_t)(0xE0 | c >> 12), b[1] = (uint8_t)(0x80 | ((c >> 6) & 63));
        b[2] = (uint8_t)(0x80 | (c & 63)), n = 3;
    } else {
        b[0] = (uint8_t)(0xF0 | c >> 18), b[1] = (uint8_t)(0x80 | ((c >> 12) & 63));
        b[2] = (uint8_t)(0x80 | ((c >> 6) & 63)), b[3] = (uint8_t)(0x80 | (c & 63)), n = 4;
    }

    if (at + n > room) return 0;

    memcpy(out + at, b, n);
    return n;
}

/* The length of a well-formed UTF-8 sequence at `p`, or 0. */
static size_t utf8_ok(const uint8_t *p, size_t left)
{
    uint8_t c = p[0];
    size_t n;
    uint32_t v;

    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) n = 2, v = c & 0x1F;
    else if (c >= 0xE0 && c <= 0xEF) n = 3, v = c & 0x0F;
    else if (c >= 0xF0 && c <= 0xF4) n = 4, v = c & 0x07;
    else return 0;

    if (left < n) return 0;

    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        v = v << 6 | (p[i] & 63);
    }

    /* Not too long a way of writing it, not a surrogate, not past Unicode. */
    if ((n == 3 && v < 0x800) || (n == 4 && v < 0x10000) || (v >= 0xD800 && v <= 0xDFFF)
        || v > 0x10FFFF) {
        return 0;
    }

    return n;
}

size_t mime_to_utf8(const char *charset, const uint8_t *in, size_t n, uint8_t *out, size_t room)
{
    char name[40];
    unsigned first = 0;
    const uint32_t *t;
    size_t o = 0;

    lower(charset ? charset : "", name, sizeof name);
    t = table_of(name, &first);

    for (size_t i = 0; i < n;) {
        size_t k;

        if (t != NULL) {
            uint32_t c = in[i] < first ? in[i] : t[in[i] - first];

            k = put_utf8(c == 0xFFFF ? 0xFFFD : c, out, o, room);
            i++;
        } else {
            /* UTF-8, US-ASCII and anything not known: taken as UTF-8, a
             * byte that is not part of a good sequence replaced. */
            size_t s = utf8_ok(in + i, n - i);

            if (s == 0) {
                k = put_utf8(0xFFFD, out, o, room);
                i++;
            } else {
                k = o + s <= room ? s : 0;
                /* memmove: `mime_part_bytes` hands the bytes from the far end
                 * of the same room this writes into. */
                if (k) memmove(out + o, in + i, s);
                i += s;
            }
        }

        if (k == 0) break;          /* out of room */

        o += k;
    }

    return o;
}
