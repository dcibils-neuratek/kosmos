/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERVERS_PACKFLAT_H
#define KOSMOS_SERVERS_PACKFLAT_H

/*
 * A flat table in `sys.pack`'s format, read and written without Lua
 * (`docs/diskfs.md` step 3).
 *
 * A file's attributes are stored as the bytes `sys.pack` makes of a table -
 * `lua/kosmos/serialize.c` is the format - and the disk server in C has to
 * read them to merge a `setattr` into them and to hold a query's terms
 * against them. **Flat tables only**: keys and values that are strings,
 * integers, floats and booleans, which is what attributes are. A nested
 * table, a nil, or bytes that are not a table are refused rather than
 * guessed at.
 *
 * Reading does not copy: a string's bytes are pointed at where they lie, so
 * a `struct packflat` is valid while the buffer it was read from is.
 *
 * `tools/test_packflat.lua` holds this to the real serialiser, on the Mac:
 * what `sys.pack` makes is read, merged and written back here, and
 * `sys.unpack` has to give the table the change describes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* serialize.c's type bytes. */
#define PACKFLAT_FALSE   1u
#define PACKFLAT_TRUE    2u
#define PACKFLAT_INT     3u
#define PACKFLAT_FLOAT   4u
#define PACKFLAT_STRING  5u

/*
 * The most entries one table holds here. A block of attributes is 4092
 * bytes and the smallest entry - a one-letter name and a boolean - is 7, so
 * a full block could in principle hold more; a file with more than this many
 * attributes is refused rather than half read.
 */
#define PACKFLAT_MAX     128u

#define PACKFLAT_OK          0
#define PACKFLAT_E_MALFORMED -1     /* not the bytes of a value */
#define PACKFLAT_E_NOT_FLAT  -2     /* a table in it, or a nil */
#define PACKFLAT_E_TOO_MANY  -3     /* more than PACKFLAT_MAX entries */
#define PACKFLAT_E_TOO_BIG   -4     /* does not fit where it is written */

struct packflat_value {
    uint8_t type;
    uint64_t bits;                  /* an integer, or a float's bits */
    const char *text;               /* a string's bytes */
    uint32_t len;
};

struct packflat {
    uint32_t count;
    struct packflat_value key[PACKFLAT_MAX];
    struct packflat_value value[PACKFLAT_MAX];
};

/* `buf` holds one packed table, and nothing after it. */
int packflat_read(const void *buf, size_t len, struct packflat *out);

int packflat_write(const struct packflat *t, void *buf, size_t cap, size_t *len);

/* The value named `name`, or NULL. Only string keys have names. */
const struct packflat_value *packflat_get(const struct packflat *t,
                                          const char *name, size_t len);

/* `name` given `value`, or taken out when `value` is NULL. */
int packflat_set(struct packflat *t, const char *name, size_t len,
                 const struct packflat_value *value);

/*
 * A value as Lua's `tostring` writes it: an integer in decimal, a float as
 * `%.14g` with ".0" when that looks like an integer, a boolean as `true` or
 * `false`, and a string as itself. A query compares values this way, as
 * the Lua server did, so `5` and `"5"` are the same answer. Answers the
 * length; `out` holds 64 bytes, or a string's length.
 */
size_t packflat_text(const struct packflat_value *v, char *out, size_t cap);

/* A value from its type and the fields that type uses. */
struct packflat_value packflat_int(int64_t n);
struct packflat_value packflat_string(const char *text, size_t len);

#endif /* KOSMOS_SERVERS_PACKFLAT_H */
