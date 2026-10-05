/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * gzip, read (RFC 1952): the members of a gzip stream inflated, each held
 * to its CRC-32 and its length, the bytes handed to whoever asked as they
 * come. `gzip.c` is the argument; this is the shape both the kit and the
 * host test compile against.
 */

#ifndef KOSMOS_GZIP_H
#define KOSMOS_GZIP_H

#include <stddef.h>
#include <stdint.h>

#include "miniz.h"

/* What reading a stream came to. Everything below `GUNZIP_WHOLE` put what
 * it could before it stopped. */
enum {
    GUNZIP_WHOLE     =  0,      /* every member inflated and checked       */
    GUNZIP_SHORT     = -1,      /* the bytes ended before the stream did    */
    GUNZIP_NOT_GZIP  = -2,      /* the first bytes are not a gzip header    */
    GUNZIP_BAD_DATA  = -3,      /* the deflate inside would not inflate     */
    GUNZIP_BAD_CHECK = -4,      /* inflated, and the CRC or length differs  */
    GUNZIP_REFUSED   = -5       /* `put` said stop: the caller's ceiling    */
};

/* Where it works: the inflater's state and its 32 KB window. The caller's,
 * so this allocates nothing - a Lua userdata in the kit, a static in the
 * test. */
struct gunzip_work {
    tinfl_decompressor inflater;
    uint8_t            window[TINFL_LZ_DICT_SIZE];
};

/* Told each piece of the output, in order; returns 0 to stop. */
typedef int (*gunzip_put)(void *user, const uint8_t *bytes, size_t n);

/* Inflates `len` bytes of gzip at `src`, putting the output as it comes.
 * `*out` is how many bytes were put. Returns one of the above. */
int kosmos_gunzip(const uint8_t *src, size_t len, struct gunzip_work *work,
                  gunzip_put put, void *user, size_t *out);

/*
 * **Plain DEFLATE, with no gzip member around it** - a zlib stream's body
 * once its two bytes of header are off, a PDF's FlateDecode, a PNG's image
 * data, a zip's entry. The same inflater, and the system's only one.
 *
 * `kosmos_inflate` writes it straight into `cap` bytes at `dst`: the results
 * above, `*out` the bytes written, and `GUNZIP_REFUSED` for a stream that
 * does not fit - `*out` then being `cap`. Only `work->inflater` is used.
 *
 * `kosmos_inflated_size` is how long it would be without keeping any of it,
 * decoded through `work`'s window.
 */
int kosmos_inflate(const uint8_t *src, size_t len, struct gunzip_work *work,
                   uint8_t *dst, size_t cap, size_t *out);

int kosmos_inflated_size(const uint8_t *src, size_t len,
                         struct gunzip_work *work, size_t *size);

/*
 * The inflater this process keeps (`inflate.c`): a `gunzip_work` in pages of
 * its own, mapped the first time it is wanted and kept, as the deflater's
 * state is - for a caller in C with nowhere of its own to put 43 KB. NULL
 * when there was no room.
 */
struct gunzip_work *kosmos_inflater(void);

/* The sentence for a result, for an error a person reads. */
const char *kosmos_gunzip_said(int result);

/*
 * **The same stream, read as it arrives** (`roadmap.md` 6zz l1): fed in
 * whatever pieces the network gave, putting what each inflates to, and
 * coming to exactly what `kosmos_gunzip` comes to over the same bytes. The
 * caller's, as `gunzip_work` is; nothing allocated.
 */
struct gunzip_stream {
    struct gunzip_work work;
    int                state;       /* where in the stream (`gzip.c`)      */
    int                result;      /* how it failed, once it has          */
    int                inflating;   /* the member's deflate begun          */
    uint8_t            flags;       /* the member's header flags           */
    uint32_t           field;       /* bytes of the header field to go     */
    unsigned           have;        /* bytes of a length or trailer so far */
    uint8_t            trailer[8];  /* the member's CRC-32 and length      */
    size_t             window_at;
    mz_ulong           crc;
    uint32_t           member_out;
    unsigned           members;     /* members whole and checked           */
    size_t             out;         /* bytes put, every member             */
};

void kosmos_gunzip_begin(struct gunzip_stream *s);

/* The next `len` bytes, their output put as it comes. `GUNZIP_WHOLE` while
 * nothing has gone wrong - which is not yet "whole", since more may come -
 * and the failure otherwise, which every later call returns again. */
int kosmos_gunzip_feed(struct gunzip_stream *s, const uint8_t *src, size_t len,
                       gunzip_put put, void *user);

/* The bytes have ended: what the stream came to, as `kosmos_gunzip`
 * would say it. */
int kosmos_gunzip_end(struct gunzip_stream *s);

#endif
