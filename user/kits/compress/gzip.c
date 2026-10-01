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

/*
 * **The same, as the bytes arrive** (`roadmap.md` 6zz l1): a page is
 * inflated while it comes and handed on to the parser a piece at a time,
 * rather than inflated whole once the last byte is in. Fed whatever the
 * network gave, in whatever pieces, and it comes to exactly what
 * `kosmos_gunzip` does over the same bytes - which `tools/test_gunzip.c`
 * holds it to, every case of that test fed one byte at a time and in
 * pieces of several sizes.
 *
 * The header is walked a byte at a time, so a name or an extra field of
 * any length costs nothing to keep; the deflate goes to `tinfl` told more
 * may come; the trailer's eight bytes are gathered wherever they fall.
 */

/* Where in the stream: the header's fields in order, then the data, the
 * trailer, the gap before another member, and the end. */
enum {
    AT_ID1, AT_ID2, AT_METHOD, AT_FLAGS, AT_REST, AT_XLEN, AT_EXTRA,
    AT_NAME, AT_COMMENT, AT_HCRC, AT_DATA, AT_TRAILER, AT_BETWEEN,
    AT_AFTER, AT_FAILED
};

void kosmos_gunzip_begin(struct gunzip_stream *s)
{
    s->state = AT_ID1;
    s->result = GUNZIP_SHORT;
    s->inflating = 0;
    s->flags = 0;
    s->field = 0;
    s->have = 0;
    s->window_at = 0;
    s->crc = MZ_CRC32_INIT;
    s->member_out = 0;
    s->members = 0;
    s->out = 0;
}

/* A header that is not one: no stream at all before the first member, and
 * bytes after the end - which gzip ignores - after it. */
static void not_a_header(struct gunzip_stream *s)
{
    if (s->members == 0) {
        s->state = AT_FAILED;
        s->result = GUNZIP_NOT_GZIP;
    } else {
        s->state = AT_AFTER;
    }
}

/* On from the field just ended to the next one the flags name, or to the
 * data - each field entered with what it counts. */
static void next_field(struct gunzip_stream *s)
{
    int was = s->state;

    s->field = 0;
    s->have = 0;

    if (was < AT_EXTRA && (s->flags & 0x04)) {
        s->state = AT_XLEN;
    } else if (was < AT_NAME && (s->flags & 0x08)) {
        s->state = AT_NAME;
    } else if (was < AT_COMMENT && (s->flags & 0x10)) {
        s->state = AT_COMMENT;
    } else if (was < AT_HCRC && (s->flags & 0x02)) {
        s->state = AT_HCRC;
        s->field = 2;
    } else {
        s->state = AT_DATA;
    }
}

int kosmos_gunzip_feed(struct gunzip_stream *s, const uint8_t *src, size_t len,
                       gunzip_put put, void *user)
{
    size_t at = 0;

    while (at < len) {
        uint8_t b = src[at];

        switch (s->state) {
        case AT_FAILED:
            return s->result;

        case AT_AFTER:
            return GUNZIP_WHOLE;

        case AT_BETWEEN:
            /* Another member's first byte, or the end and what gzip
             * ignores after it. */
            if (b != 0x1f) {
                s->state = AT_AFTER;
                return GUNZIP_WHOLE;
            }

            s->state = AT_ID2;
            at++;
            break;

        case AT_ID1:
            if (b != 0x1f) { not_a_header(s); break; }
            s->state = AT_ID2;
            at++;
            break;

        case AT_ID2:
            if (b != 0x8b) { not_a_header(s); break; }
            s->state = AT_METHOD;
            at++;
            break;

        case AT_METHOD:
            if (b != 8) { not_a_header(s); break; }
            s->state = AT_FLAGS;
            at++;
            break;

        case AT_FLAGS:
            s->flags = b;
            s->field = 6;                   /* the time, XFL and OS */
            s->state = AT_REST;
            at++;
            break;

        case AT_REST: {
            size_t take = len - at < s->field ? len - at : s->field;

            at += take;
            s->field -= (uint32_t)take;

            if (s->field == 0) {
                /* Reserved flags refused once the ten bytes are here, as
                 * the whole form refuses them. */
                if (s->flags & 0xe0) { not_a_header(s); break; }

                next_field(s);
            }
            break;
        }

        case AT_XLEN:
            s->field |= (uint32_t)b << (8 * s->have);
            at++;

            if (++s->have == 2) {
                s->state = AT_EXTRA;
            }
            break;

        case AT_EXTRA: {
            size_t take = len - at < s->field ? len - at : s->field;

            at += take;
            s->field -= (uint32_t)take;

            if (s->field == 0) {
                next_field(s);
            }
            break;
        }

        case AT_NAME:
        case AT_COMMENT: {
            const uint8_t *end = memchr(src + at, 0, len - at);

            if (end == NULL) {
                at = len;
                break;
            }

            at = (size_t)(end - src) + 1;
            next_field(s);
            break;
        }

        case AT_HCRC:
            at++;

            if (--s->field == 0) {
                s->state = AT_DATA;
            }
            break;

        case AT_DATA:
            break;

        case AT_TRAILER:
            s->trailer[s->have++] = b;
            at++;

            if (s->have == 8) {
                if (le32(s->trailer) != (uint32_t)s->crc
                    || le32(s->trailer + 4) != s->member_out) {
                    s->state = AT_FAILED;
                    s->result = GUNZIP_BAD_CHECK;
                    return s->result;
                }

                s->members++;
                s->state = AT_BETWEEN;
            }
            break;
        }

        if (s->state == AT_FAILED) {
            return s->result;
        }

        if (s->state == AT_AFTER) {
            return GUNZIP_WHOLE;
        }

        /* The data: from the header's end, to the deflate's. */
        if (s->state == AT_DATA) {
            if (!s->inflating) {
                tinfl_init(&s->work.inflater);
                s->inflating = 1;
                s->window_at = 0;
                s->crc = MZ_CRC32_INIT;
                s->member_out = 0;
            }

            for (;;) {
                size_t in = len - at;
                size_t room = TINFL_LZ_DICT_SIZE - s->window_at;
                tinfl_status status;

                status = tinfl_decompress(&s->work.inflater, src + at, &in,
                                          s->work.window,
                                          s->work.window + s->window_at, &room,
                                          TINFL_FLAG_HAS_MORE_INPUT);
                at += in;

                if (room > 0) {
                    s->crc = mz_crc32(s->crc, s->work.window + s->window_at, room);
                    s->member_out += (uint32_t)room;
                    s->out += room;

                    if (!put(user, s->work.window + s->window_at, room)) {
                        s->state = AT_FAILED;
                        s->result = GUNZIP_REFUSED;
                        return s->result;
                    }
                }

                s->window_at = (s->window_at + room) & (TINFL_LZ_DICT_SIZE - 1);

                if (status == TINFL_STATUS_DONE) {
                    s->inflating = 0;
                    s->state = AT_TRAILER;
                    s->have = 0;
                    break;
                }

                if (status == TINFL_STATUS_HAS_MORE_OUTPUT) {
                    continue;
                }

                /* Everything given taken, and waiting for the rest - or, in
                 * case it ever stops short of the end, called again while
                 * it is still moving. */
                if (status == TINFL_STATUS_NEEDS_MORE_INPUT) {
                    if (at == len) {
                        return GUNZIP_WHOLE;
                    }

                    if (in > 0 || room > 0) {
                        continue;
                    }
                }

                s->state = AT_FAILED;
                s->result = GUNZIP_BAD_DATA;
                return s->result;
            }
        }
    }

    return GUNZIP_WHOLE;
}

int kosmos_gunzip_end(struct gunzip_stream *s)
{
    if (s->state == AT_FAILED) {
        return s->result;
    }

    if (s->state == AT_BETWEEN || s->state == AT_AFTER) {
        return GUNZIP_WHOLE;
    }

    return GUNZIP_SHORT;
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
