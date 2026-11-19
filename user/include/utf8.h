/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_UTF8_H
#define KOSMOS_UTF8_H

/*
 * One character in UTF-8, read or written (RFC 3629).
 *
 * Six kits each had their own - the Graphics Kit's text and its faces, the
 * LLM Kit's tokenizer, the Mail Kit's charsets, the SMB Kit's names - and
 * they disagreed about what was malformed: some took C0 and C1 for a lead
 * byte, some F5 to F7, and only Mail's refused a character written longer
 * than it needs, a surrogate, or one past U+10FFFF. This is the one copy
 * (`cleanup.md` finding 104), with Mail's rule for all of them.
 *
 * A byte at a time and inline, as `bytes.h` is, so a text loop pays a call
 * for none of it.
 */

#include <stddef.h>
#include <stdint.h>

/*
 * How long the well-formed character at `p` is, 1 to 4, or 0 when it is not
 * one: a stray continuation byte, a lead byte that begins nothing, one cut
 * short by the end at `left`, a character written longer than it needs, a
 * surrogate, or one past U+10FFFF.
 */
static inline size_t utf8_valid(const uint8_t *p, size_t left)
{
    uint8_t c = p[0];
    uint32_t v;
    size_t n, i;

    if (left == 0) return 0;
    if (c < 0x80u) return 1;

    if (c >= 0xC2u && c <= 0xDFu)      n = 2, v = c & 0x1Fu;
    else if (c >= 0xE0u && c <= 0xEFu) n = 3, v = c & 0x0Fu;
    else if (c >= 0xF0u && c <= 0xF4u) n = 4, v = c & 0x07u;
    else return 0;

    if (left < n) return 0;

    for (i = 1; i < n; i++) {
        if ((p[i] & 0xC0u) != 0x80u) return 0;
        v = v << 6 | (p[i] & 0x3Fu);
    }

    if ((n == 3 && v < 0x800u) || (n == 4 && v < 0x10000u)
        || (v >= 0xD800u && v <= 0xDFFFu) || v > 0x10FFFFu) {
        return 0;
    }

    return n;
}

/*
 * The character at `p`, and in `*len` how many bytes it took. What is not
 * well-formed reads as U+FFFD one byte long, so a loop over bad text still
 * moves on, a byte at a time, and draws or counts a replacement for each.
 */
static inline uint32_t utf8_get(const uint8_t *p, size_t left, size_t *len)
{
    size_t n = utf8_valid(p, left), i;
    uint32_t v;

    if (n == 0) {
        *len = 1;
        return 0xFFFDu;
    }

    *len = n;

    if (n == 1) return p[0];

    v = p[0] & (0x7Fu >> n);

    for (i = 1; i < n; i++) {
        v = v << 6 | (p[i] & 0x3Fu);
    }

    return v;
}

/* `c` written into `out`, which has room for four: how many bytes it took.
 * The caller says what is a character; this only writes one. */
static inline size_t utf8_put(uint32_t c, uint8_t out[4])
{
    if (c < 0x80u) {
        out[0] = (uint8_t)c;
        return 1;
    }

    if (c < 0x800u) {
        out[0] = (uint8_t)(0xC0u | c >> 6);
        out[1] = (uint8_t)(0x80u | (c & 0x3Fu));
        return 2;
    }

    if (c < 0x10000u) {
        out[0] = (uint8_t)(0xE0u | c >> 12);
        out[1] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | (c & 0x3Fu));
        return 3;
    }

    out[0] = (uint8_t)(0xF0u | c >> 18);
    out[1] = (uint8_t)(0x80u | ((c >> 12) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
    out[3] = (uint8_t)(0x80u | (c & 0x3Fu));
    return 4;
}

#endif /* KOSMOS_UTF8_H */
