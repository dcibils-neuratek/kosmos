/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * gzip, read - for the web's pages, which arrive compressed when the
 * browser says it can take them so (`roadmap.md` 6zz g).
 *
 * **Why it is worth having.** Wikipedia's Dam article is 1.4 MB of HTML
 * and about a fifth of that gzipped, and the fetch was half of the two
 * seconds the page took (`testing.md` 18.305). Every server on the web
 * compresses for a client that asks, and a client that never asks pays for
 * the whole of every page, every time.
 *
 * **A gzip stream** (RFC 1952) is one or more members, each a header of ten
 * bytes and whatever its flags add - extra bytes, a name, a comment, a
 * CRC-16 of the header - then raw DEFLATE, then the CRC-32 of what it
 * inflates to and that length modulo 2^32. Each member is held to both:
 * a page that inflates to something else is not the page.
 *
 * **The inflater is miniz's `tinfl`**, vendored in `runtime/upstream/miniz/`
 * beside the deflater the zip writer uses, and run the way miniz's own
 * `tinfl_decompress_mem_to_callback` runs it: into a 32 KB window that
 * wraps, each stretch of output handed on before the window comes round
 * again. So nothing here knows or trusts how long the result will be -
 * the length in the trailer is checked, never used to size anything - and
 * the caller decides where the bytes go and when there are too many.
 * `puff`, which `inflate` uses, decodes a stream twice to learn its size and
 * is the slow reference inflater its name says it is.
 *
 * No Lua in this file, so `tools/test_gunzip.c` compiles it on the Mac and
 * holds it to what Python's `gzip` writes.
 */

#include "gzip.h"

#include <string.h>

/*
 * The header's length, from its flags; 0 when the bytes end inside it, and
 * `(size_t)-1` when they are not one. Reserved flags set are refused, as
 * the RFC says a reader must.
 */
static size_t header(const uint8_t *p, size_t n)
{
    size_t at = 10;
    uint8_t flags;

    if (n >= 1 && p[0] != 0x1f) return (size_t)-1;
    if (n >= 2 && p[1] != 0x8b) return (size_t)-1;
    if (n >= 3 && p[2] != 8) return (size_t)-1;          /* deflate, only */
    if (n < 10) return 0;

    flags = p[3];

    if (flags & 0xe0) return (size_t)-1;

    if (flags & 0x04) {                                   /* FEXTRA   */
        if (at + 2 > n) return 0;
        at += 2 + ((size_t)p[at] | (size_t)p[at + 1] << 8);
    }

    if (flags & 0x08) {                                   /* FNAME    */
        const uint8_t *end = at < n ? memchr(p + at, 0, n - at) : NULL;

        if (end == NULL) return 0;
        at = (size_t)(end - p) + 1;
    }

    if (flags & 0x10) {                                   /* FCOMMENT */
        const uint8_t *end = at < n ? memchr(p + at, 0, n - at) : NULL;

        if (end == NULL) return 0;
        at = (size_t)(end - p) + 1;
    }

    if (flags & 0x02) {                                   /* FHCRC    */
        at += 2;
    }

    return at <= n ? at : 0;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
           | (uint32_t)p[3] << 24;
}

int kosmos_gunzip(const uint8_t *src, size_t len, struct gunzip_work *work,
                  gunzip_put put, void *user, size_t *out)
{
    size_t at = 0;
    int members = 0;

    *out = 0;

    for (;;) {
        size_t h, window_at = 0, member_out = 0;
        mz_ulong crc = MZ_CRC32_INIT;

        /*
         * Another member, or the end. Bytes after the last member that are
         * not a header are what `gzip` calls trailing garbage and ignores,
         * and so does this: a server that pads its reply has still sent the
         * page.
         */
        if (members > 0 && (at >= len || src[at] != 0x1f)) {
            return GUNZIP_WHOLE;
        }

        h = header(src + at, len - at);

        if (h == (size_t)-1) {
            return members == 0 ? GUNZIP_NOT_GZIP : GUNZIP_WHOLE;
        }

        if (h == 0) {
            return GUNZIP_SHORT;
        }

        at += h;
        tinfl_init(&work->inflater);

        for (;;) {
            size_t in = len - at;
            size_t room = TINFL_LZ_DICT_SIZE - window_at;
            tinfl_status status;

            /* No flags: raw deflate, every byte there is already here, and
             * a window that wraps. */
            status = tinfl_decompress(&work->inflater, src + at, &in,
                                      work->window, work->window + window_at,
                                      &room, 0);
            at += in;

            if (room > 0) {
                crc = mz_crc32(crc, work->window + window_at, room);
                member_out += room;
                *out += room;

                if (!put(user, work->window + window_at, room)) {
                    return GUNZIP_REFUSED;
                }
            }

            window_at = (window_at + room) & (TINFL_LZ_DICT_SIZE - 1);

            if (status == TINFL_STATUS_DONE) {
                break;
            }

            if (status == TINFL_STATUS_HAS_MORE_OUTPUT) {
                continue;
            }

            /* It wanted more than there was: a reply cut off. */
            if (status == TINFL_STATUS_NEEDS_MORE_INPUT
                || status == TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS) {
                return GUNZIP_SHORT;
            }

            return GUNZIP_BAD_DATA;
        }

        if (len - at < 8) {
            return GUNZIP_SHORT;
        }

        if (le32(src + at) != (uint32_t)crc
            || le32(src + at + 4) != (uint32_t)member_out) {
            return GUNZIP_BAD_CHECK;
        }

        at += 8;
        members++;
    }
}

const char *kosmos_gunzip_said(int result)
{
    switch (result) {
    case GUNZIP_WHOLE:     return "whole";
    case GUNZIP_SHORT:     return "the gzip stream was cut short";
    case GUNZIP_NOT_GZIP:  return "that is not gzip";
    case GUNZIP_BAD_DATA:  return "the gzip stream's data would not inflate";
    case GUNZIP_BAD_CHECK: return "the gzip stream inflated to something its check does not match";
    case GUNZIP_REFUSED:   return "the gzip stream inflates past what this machine has room for";
    default:               return "gzip went wrong in a way nobody named";
    }
}
