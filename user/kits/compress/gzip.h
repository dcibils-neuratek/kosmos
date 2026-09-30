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

/* The sentence for a result, for an error a person reads. */
const char *kosmos_gunzip_said(int result);

#endif
