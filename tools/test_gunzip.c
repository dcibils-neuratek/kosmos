/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `gzip.c`, held to gzip on the Mac (`roadmap.md` 6zz g).
 *
 * One stream Python's `gzip` wrote - a name in its header, level 9 - so the
 * reader is held to somebody else's writer and not only to its own idea of
 * the format; and streams built here with miniz's deflater and headers
 * written by hand, for every flag the header has, two members, bytes after
 * the end, a megabyte, every way of being cut short, a CRC and a length
 * that lie, data that is not deflate, and a caller that says stop.
 *
 * **And each of those read as it arrives** (`roadmap.md` 6zz l1): the
 * stream form fed the same bytes a byte at a time and in pieces of several
 * sizes, held to the whole form's answer and bytes every time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gzip.h"

static int failures;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

/* Written by Python 3: `gzip.GzipFile(filename="page.html", mtime=0,
 * compresslevel=9)` over the 200 lines `python_text` makes. */
static const uint8_t python_gz[] = {
    0x1f, 0x8b, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x70, 0x61,
    0x67, 0x65, 0x2e, 0x68, 0x74, 0x6d, 0x6c, 0x00, 0x95, 0xd8, 0xbb, 0x71,
    0x14, 0x51, 0x00, 0x44, 0x51, 0x9f, 0x28, 0xc6, 0xc3, 0xc1, 0xe0, 0xce,
    0x9b, 0x6f, 0x16, 0xa4, 0x20, 0x89, 0x45, 0xa8, 0x4a, 0x68, 0xb7, 0x58,
    0x1c, 0x11, 0x3d, 0x18, 0x04, 0xc0, 0x69, 0xbf, 0xad, 0xeb, 0x9d, 0xd7,
    0x97, 0xb7, 0xcb, 0xf4, 0xf9, 0xef, 0xa6, 0xeb, 0xb7, 0xe9, 0x61, 0xba,
    0x3d, 0x3c, 0x5f, 0x3e, 0x4d, 0x4f, 0xd7, 0x1f, 0xb7, 0x9f, 0x97, 0xfb,
    0xfd, 0xf2, 0x75, 0x7a, 0x7c, 0x9f, 0xbe, 0xbc, 0xff, 0xfa, 0x7e, 0x7d,
    0xfb, 0x78, 0x9f, 0x9e, 0x7f, 0xbf, 0xdc, 0x3e, 0xbc, 0xfe, 0x3b, 0xa4,
    0x87, 0x59, 0x0f, 0x43, 0x0f, 0x8b, 0x1e, 0x56, 0x3d, 0x6c, 0x7a, 0xd8,
    0xf5, 0x70, 0xe8, 0xe1, 0xc4, 0x43, 0x5a, 0x3a, 0x2d, 0x9d, 0x96, 0x4e,
    0x4b, 0xa7, 0xa5, 0xd3, 0xd2, 0x69, 0xe9, 0xb4, 0x74, 0x5a, 0x3a, 0x2d,
    0x3d, 0x6b, 0xe9, 0x59, 0x4b, 0xcf, 0x5a, 0x7a, 0xd6, 0xd2, 0xb3, 0x96,
    0x9e, 0xb5, 0xf4, 0xac, 0xa5, 0x67, 0x2d, 0x3d, 0x6b, 0xe9, 0x59, 0x4b,
    0x0f, 0x2d, 0x3d, 0xb4, 0xf4, 0xd0, 0xd2, 0x43, 0x4b, 0x0f, 0x2d, 0x3d,
    0xb4, 0xf4, 0xd0, 0xd2, 0x43, 0x4b, 0x0f, 0x2d, 0x3d, 0xb4, 0xf4, 0xa2,
    0xa5, 0x17, 0x2d, 0xbd, 0x68, 0xe9, 0x45, 0x4b, 0x2f, 0x5a, 0x7a, 0xd1,
    0xd2, 0x8b, 0x96, 0x5e, 0xb4, 0xf4, 0xa2, 0xa5, 0x17, 0x2d, 0xbd, 0x6a,
    0xe9, 0x55, 0x4b, 0xaf, 0x5a, 0x7a, 0xd5, 0xd2, 0xab, 0x96, 0x5e, 0xb5,
    0xf4, 0xaa, 0xa5, 0x57, 0x2d, 0xbd, 0x6a, 0xe9, 0x55, 0x4b, 0x6f, 0x5a,
    0x7a, 0xd3, 0xd2, 0x9b, 0x96, 0xde, 0xb4, 0xf4, 0xa6, 0xa5, 0x37, 0x2d,
    0xbd, 0x69, 0xe9, 0x4d, 0x4b, 0x6f, 0x5a, 0x7a, 0xd3, 0xd2, 0xbb, 0x96,
    0xde, 0xb5, 0xf4, 0xae, 0xa5, 0x77, 0x2d, 0xbd, 0x6b, 0xe9, 0x5d, 0x4b,
    0xef, 0x5a, 0x7a, 0xd7, 0xd2, 0xbb, 0x96, 0xde, 0xb5, 0xf4, 0xa1, 0xa5,
    0x0f, 0x2d, 0x7d, 0x68, 0xe9, 0x43, 0x4b, 0x1f, 0x5a, 0xfa, 0xd0, 0xd2,
    0x87, 0x96, 0x3e, 0xb4, 0xf4, 0xa1, 0xa5, 0x0f, 0x2d, 0x7d, 0x6a, 0xe9,
    0x53, 0x4b, 0x9f, 0x5a, 0xfa, 0xd4, 0xd2, 0xa7, 0x96, 0x3e, 0xb5, 0xf4,
    0xa9, 0xa5, 0x4f, 0x2d, 0x7d, 0x6a, 0xe9, 0x13, 0x4b, 0xa7, 0x46, 0x96,
    0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a,
    0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91,
    0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96,
    0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a,
    0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91,
    0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96,
    0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a,
    0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91,
    0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96,
    0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a,
    0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91,
    0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96,
    0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a,
    0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91,
    0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96,
    0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a,
    0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91,
    0xa5, 0x46, 0x96, 0x1a, 0x59, 0x6a, 0x64, 0xa9, 0x91, 0xa5, 0x46, 0x96,
    0x1a, 0x59, 0x6a, 0x64, 0xfd, 0xbf, 0x91, 0xfd, 0x01, 0xe8, 0xd7, 0x7e,
    0x4c, 0x48, 0x26, 0x00, 0x00,
};

static size_t python_text(uint8_t *out)
{
    size_t n = 0;
    int i;

    for (i = 0; i < 200; i++) {
        n += (size_t)sprintf((char *)out + n,
                             "line %04d of a page, compressed by Python's gzip\n", i);
    }

    return n;
}

/* What `put` was given, all of it, and in how many pieces. */
struct sink {
    uint8_t *bytes;
    size_t n, cap, pieces, largest, stop_after;
};

static int put(void *user, const uint8_t *p, size_t n)
{
    struct sink *s = user;

    if (s->stop_after && s->n + n > s->stop_after) {
        return 0;
    }

    if (s->n + n > s->cap) {
        s->cap = (s->n + n) * 2;
        s->bytes = realloc(s->bytes, s->cap);
    }

    memcpy(s->bytes + s->n, p, n);
    s->n += n;
    s->pieces++;

    if (n > s->largest) s->largest = n;

    return 1;
}

static struct gunzip_work work;
static struct gunzip_stream stream;
static tdefl_compressor deflater;

/*
 * The stream form over the same bytes, fed `piece` at a time: what it put
 * and how it ended. A ceiling the sink was given is kept.
 */
static int run_stream(const uint8_t *gz, size_t len, size_t piece,
                      struct sink *s)
{
    size_t at = 0, stop_after = s->stop_after;
    int r = GUNZIP_WHOLE;

    memset(s, 0, sizeof(*s));
    s->stop_after = stop_after;
    kosmos_gunzip_begin(&stream);

    while (at < len && r == GUNZIP_WHOLE) {
        size_t n = len - at < piece ? len - at : piece;

        r = kosmos_gunzip_feed(&stream, gz + at, n, put, s);
        at += n;
    }

    return r == GUNZIP_WHOLE ? kosmos_gunzip_end(&stream) : r;
}

/*
 * Every case, both ways (`roadmap.md` 6zz l1): the whole form, and the
 * stream form fed a byte at a time and in pieces of several sizes - each
 * held to the whole form's answer and to its bytes, exactly. A byte at a
 * time only where that is quick; the megabyte's cuts in two sizes.
 */
static int stream_checks;

static int run(const uint8_t *gz, size_t len, struct sink *s)
{
    static const size_t SMALL[] = { 1, 2, 7, 333, 4096 };
    static const size_t LARGE[] = { 333, 65536 };
    const size_t *pieces = len <= 100000 ? SMALL : LARGE;
    size_t count = len <= 100000 ? sizeof(SMALL) / sizeof(SMALL[0])
                                 : sizeof(LARGE) / sizeof(LARGE[0]);
    size_t out = 0, i;
    int r;

    memset(s, 0, sizeof(*s));
    r = kosmos_gunzip(gz, len, &work, put, s, &out);

    CHECK(out == s->n, "reported %zu bytes out and put %zu", out, s->n);

    for (i = 0; i < count; i++) {
        struct sink t = { 0 };
        int rs = run_stream(gz, len, pieces[i], &t);

        CHECK(rs == r, "%zu bytes fed %zu at a time: %s, where whole: %s",
              len, pieces[i], kosmos_gunzip_said(rs), kosmos_gunzip_said(r));
        CHECK(t.n == s->n && (t.n == 0 || memcmp(t.bytes, s->bytes, t.n) == 0),
              "%zu bytes fed %zu at a time put %zu bytes, where whole put %zu",
              len, pieces[i], t.n, s->n);
        CHECK(stream.out == t.n, "the stream reported %zu bytes and put %zu",
              stream.out, t.n);
        stream_checks += 3;
        free(t.bytes);
    }

    return r;
}

/* Raw deflate of `n` bytes, into `out`; its length. */
static size_t deflate_raw(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t in_n = n, out_n = cap;

    tdefl_init(&deflater, NULL, NULL, 256 | TDEFL_GREEDY_PARSING_FLAG);
    CHECK(tdefl_compress(&deflater, in, &in_n, out, &out_n, TDEFL_FINISH)
          == TDEFL_STATUS_DONE, "tdefl did not finish");

    return out_n;
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* A member: a header with `flags`' fields, the deflate, the trailer. */
static size_t member(const uint8_t *in, size_t n, uint8_t flags, uint8_t *out,
                     size_t cap)
{
    size_t at = 0;
    uint8_t head[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3 };

    head[3] = flags;
    memcpy(out, head, 10);
    at = 10;

    if (flags & 0x04) {
        out[at++] = 5; out[at++] = 0;
        memcpy(out + at, "xtra!", 5);
        at += 5;
    }

    if (flags & 0x08) {
        memcpy(out + at, "a name.html", 12);
        at += 12;
    }

    if (flags & 0x10) {
        memcpy(out + at, "a comment", 10);
        at += 10;
    }

    if (flags & 0x02) {
        out[at++] = 0xab; out[at++] = 0xcd;
    }

    at += deflate_raw(in, n, out + at, cap - at - 8);
    le32(out + at, (uint32_t)mz_crc32(MZ_CRC32_INIT, in, n));
    le32(out + at + 4, (uint32_t)n);

    return at + 8;
}

int main(void)
{
    static uint8_t text[1 << 20], gz[(1 << 21)], other[4096];
    struct sink s;
    size_t n, len, cut, a_len;
    int r, checks = 0;
    uint32_t seed = 12345;

    /* Python's, whole: its name skipped, its CRC and length held. */
    n = python_text(text);
    r = run(python_gz, sizeof(python_gz), &s);
    CHECK(r == GUNZIP_WHOLE, "Python's stream: %s", kosmos_gunzip_said(r));
    CHECK(s.n == n && memcmp(s.bytes, text, n) == 0,
          "Python's stream inflated to %zu bytes, not its %zu", s.n, n);
    checks += 2;
    free(s.bytes);

    /* A megabyte of text that repeats and does not, with every flag. */
    for (n = 0; n < sizeof(text); n++) {
        seed = seed * 1103515245u + 12345u;
        text[n] = (n % 97 < 60) ? (uint8_t)("the web, compressed "[n % 20])
                                : (uint8_t)('a' + (seed >> 16) % 26);
    }

    len = member(text, n, 0x04 | 0x08 | 0x10 | 0x02, gz, sizeof(gz));
    r = run(gz, len, &s);
    CHECK(r == GUNZIP_WHOLE, "every flag: %s", kosmos_gunzip_said(r));
    CHECK(s.n == n && memcmp(s.bytes, text, n) == 0,
          "every flag: %zu bytes of %zu, or different ones", s.n, n);
    CHECK(s.largest <= TINFL_LZ_DICT_SIZE && s.pieces >= n / TINFL_LZ_DICT_SIZE,
          "put %zu pieces, the largest %zu: not a window at a time",
          s.pieces, s.largest);
    checks += 3;
    free(s.bytes);

    /* Each flag alone, so each field is reached straight from the ten
     * fixed bytes - the header's CRC with nothing before it included. */
    {
        static const uint8_t ALONE[] = { 0x02, 0x04, 0x08, 0x10 };
        size_t f;

        for (f = 0; f < sizeof(ALONE); f++) {
            size_t one = member(text, 20000, ALONE[f], gz, sizeof(gz));

            r = run(gz, one, &s);
            CHECK(r == GUNZIP_WHOLE && s.n == 20000 && memcmp(s.bytes, text, 20000) == 0,
                  "flag 0x%02x alone: %s, %zu bytes", ALONE[f], kosmos_gunzip_said(r), s.n);
            free(s.bytes);
            checks++;
        }
    }

    len = member(text, n, 0x04 | 0x08 | 0x10 | 0x02, gz, sizeof(gz));

    /* Every way of being cut short: in the header, the name, the data and
     * the trailer - and what was put is the text's beginning. */
    for (cut = 1; cut < len; cut = cut < 64 ? cut + 1 : cut * 3 / 2) {
        r = run(gz, cut, &s);
        CHECK(r == GUNZIP_SHORT, "cut at %zu of %zu: %s", cut, len,
              kosmos_gunzip_said(r));
        CHECK(s.n <= n && (s.n == 0 || memcmp(s.bytes, text, s.n) == 0),
              "cut at %zu: put %zu bytes that are not the text's beginning",
              cut, s.n);
        free(s.bytes);
    }

    for (cut = len - 8; cut < len; cut++) {
        r = run(gz, cut, &s);
        CHECK(r == GUNZIP_SHORT, "cut in the trailer at %zu: %s", cut,
              kosmos_gunzip_said(r));
        CHECK(s.n == n, "cut in the trailer: the whole body was not put");
        free(s.bytes);
    }

    checks += 3;

    /* Two members, one after the other: one text. Then bytes after the
     * end, which gzip ignores. */
    a_len = member(text, 3000, 0, gz, sizeof(gz));
    len = a_len + member(text + 3000, 5000, 0x08, gz + a_len, sizeof(gz) - a_len);
    r = run(gz, len, &s);
    CHECK(r == GUNZIP_WHOLE && s.n == 8000 && memcmp(s.bytes, text, 8000) == 0,
          "two members: %s, %zu bytes", kosmos_gunzip_said(r), s.n);
    free(s.bytes);

    memset(gz + len, 0, 16);
    r = run(gz, len + 16, &s);
    CHECK(r == GUNZIP_WHOLE && s.n == 8000,
          "sixteen zeros after the end: %s, %zu bytes", kosmos_gunzip_said(r), s.n);
    free(s.bytes);

    /* Bytes after the end that begin as a header would and are not one:
     * still the end, since a member came before them - and the same bytes
     * with no member before them are not gzip. */
    gz[len] = 0x1f;
    r = run(gz, len + 16, &s);
    CHECK(r == GUNZIP_WHOLE && s.n == 8000,
          "0x1f and zeros after the end: %s, %zu bytes", kosmos_gunzip_said(r), s.n);
    free(s.bytes);
    r = run(gz + len, 16, &s);
    CHECK(r == GUNZIP_NOT_GZIP && s.n == 0,
          "0x1f and zeros alone: %s", kosmos_gunzip_said(r));
    free(s.bytes);
    checks += 4;

    /* A CRC that lies, a length that lies. */
    len = member(text, 5000, 0, gz, sizeof(gz));
    gz[len - 8] ^= 1;
    r = run(gz, len, &s);
    CHECK(r == GUNZIP_BAD_CHECK, "a CRC one bit off: %s", kosmos_gunzip_said(r));
    free(s.bytes);
    gz[len - 8] ^= 1;
    gz[len - 1] ^= 0x80;
    r = run(gz, len, &s);
    CHECK(r == GUNZIP_BAD_CHECK, "a length that lies: %s", kosmos_gunzip_said(r));
    free(s.bytes);
    gz[len - 1] ^= 0x80;
    checks += 2;

    /* Not gzip; reserved flags; a method that is not deflate; and data
     * that is not deflate at all. */
    r = run((const uint8_t *)"<!doctype html>", 15, &s);
    CHECK(r == GUNZIP_NOT_GZIP, "HTML: %s", kosmos_gunzip_said(r));
    free(s.bytes);

    memcpy(other, gz, 10);
    other[3] = 0x20;
    r = run(other, 10, &s);
    CHECK(r == GUNZIP_NOT_GZIP, "a reserved flag: %s", kosmos_gunzip_said(r));
    free(s.bytes);

    other[3] = 0;
    other[2] = 7;
    r = run(other, 10, &s);
    CHECK(r == GUNZIP_NOT_GZIP, "method 7: %s", kosmos_gunzip_said(r));
    free(s.bytes);

    memcpy(other, gz, 10);
    memset(other + 10, 0xff, 200);
    r = run(other, 210, &s);
    CHECK(r == GUNZIP_BAD_DATA, "0xff for data: %s", kosmos_gunzip_said(r));
    free(s.bytes);
    checks += 4;

    /* A caller that says stop, at a ceiling of its own. */
    len = member(text, sizeof(text), 0, gz, sizeof(gz));
    memset(&s, 0, sizeof(s));
    s.stop_after = 100000;
    {
        size_t out = 0;

        r = kosmos_gunzip(gz, len, &work, put, &s, &out);
    }
    CHECK(r == GUNZIP_REFUSED && s.n <= 100000,
          "stopped at 100000: %s, %zu bytes", kosmos_gunzip_said(r), s.n);
    free(s.bytes);

    /* And the stream form told stop: refused, and refused again after. */
    memset(&s, 0, sizeof(s));
    s.stop_after = 100000;
    r = run_stream(gz, len, 4096, &s);
    CHECK(r == GUNZIP_REFUSED && s.n <= 100000
          && kosmos_gunzip_feed(&stream, gz, 1, put, &s) == GUNZIP_REFUSED
          && kosmos_gunzip_end(&stream) == GUNZIP_REFUSED,
          "the stream stopped at 100000: %s, %zu bytes", kosmos_gunzip_said(r), s.n);
    free(s.bytes);
    checks += 2;

    checks += stream_checks;

    if (failures) {
        printf("test_gunzip: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("test_gunzip: %d checks passed, %d of them the stream form against "
           "the whole\n", checks, stream_checks);
    return 0;
}
