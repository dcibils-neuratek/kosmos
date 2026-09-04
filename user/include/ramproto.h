/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_RAMPROTO_H
#define KOSMOS_RAMPROTO_H

#include <stdint.h>

/*
 * /data: a filesystem that lives in memory, written down.
 *
 * The largest of these headers, because ramfs is the only server here that
 * is a filesystem *and* an attribute store *and* a query engine. Every piece
 * of it has a precedent in one of the four before it, though:
 *
 *   - attributes are `/dev`'s problem - a fixed number of named scalars,
 *     each a fixed size, so a variable shape travels in a fixed one;
 *   - listings are `/bin`'s - a page of entries at an offset, because a
 *     directory can hold more names than a message can carry;
 *   - `watch` is the console's - it does not answer when it is asked, and
 *     the server keeps the sender until the answer changes.
 *
 * **Both structs carry a union**, which none of the others needed. A reply
 * holds bytes, or entries, or attributes, and never two of them; a request
 * holds bytes or a list of known paths. Without the union the two would be
 * 2.7 KB and a message is 2048, and padding a message with a kilobyte that
 * every operation ignores is a kilobyte copied twice per call.
 */

#define RAM_OP_LIST      1u
#define RAM_OP_READ      2u
#define RAM_OP_WRITE     3u
#define RAM_OP_GETATTR   4u
#define RAM_OP_SETATTR   5u
#define RAM_OP_QUERY     6u
#define RAM_OP_WATCH     7u
#define RAM_OP_WATCHERS  8u

#define RAM_OK               0u
#define RAM_ERR_NO_PATH      1u
#define RAM_ERR_NOT_DIR      2u
#define RAM_ERR_NOT_READABLE 3u
#define RAM_ERR_BAD_OP       4u
#define RAM_ERR_FULL         5u   /* the node pool, or a path too long */
#define RAM_ERR_TOO_MANY     6u   /* more attributes than a node may hold */

#define RAM_PATH_MAX     128u     /* a whole path, not one component */
#define RAM_NAME_MAX      32u     /* an attribute's name */
#define RAM_VALUE_MAX     48u     /* an attribute's value, as text */
#define RAM_ATTRS_MAX      8u     /* attributes on one node, or query terms */
#define RAM_ENTRIES_MAX    8u     /* names or paths in one page of a reply */
#define RAM_DATA_MAX    1024u     /* bytes of content in one message */

/*
 * An attribute, as text with a note saying what it was.
 *
 * `/dev` settled this shape and the reasoning carries: a value is a number
 * or a string, the wire carries the characters either way, and `kind` says
 * which so the far side can hand back the type that went in. A union of
 * `double` and a buffer would save nothing - the buffer decides the size -
 * and would put a floating-point field in a struct the kernel copies.
 */
#define RAM_ATTR_TEXT    0u
#define RAM_ATTR_NUMBER  1u

struct ram_attr {
    uint32_t kind;
    char     name[RAM_NAME_MAX];
    char     value[RAM_VALUE_MAX];
};

/*
 * Whether the bytes of a value are text or a serialised Lua value.
 *
 * /data is a *value* store, not a byte store, and that is a documented
 * property rather than an accident: `help("fs")` promises that a read gives
 * back the table you wrote, integers still integers and floats still floats.
 * The Lua ramfs got it for free by keeping the deserialised value in a table.
 * A C server cannot hold a Lua value, so it holds the serialised bytes and
 * this field records which they are; the namespace packs and unpacks, and the
 * server never has to know what a table is.
 *
 * It also decides the `size` attribute, which a write maintains only for
 * text - exactly as the Lua one did, where `size` was set only when the
 * value was a string.
 */
#define RAM_RAW      0u
#define RAM_PACKED   1u

struct ram_request {
    uint32_t op;
    uint32_t offset;          /* which page of a listing, or byte of a read */
    uint32_t length;          /* bytes of `u.data`, for write */
    uint32_t count;           /* terms in `attrs`, or paths in `u.known` */
    uint32_t packed;          /* RAM_RAW or RAM_PACKED */

    char     path[RAM_PATH_MAX];

    /* setattr's values, or query and watch's conditions. */
    struct ram_attr attrs[RAM_ATTRS_MAX];

    union {
        char data[RAM_DATA_MAX];
        char known[RAM_ENTRIES_MAX][RAM_PATH_MAX];
    } u;
};

struct ram_reply {
    uint32_t error;
    uint32_t more;            /* another page follows this one */
    uint32_t count;           /* entries or attributes in `u` */
    uint32_t length;          /* bytes of `u.data` */
    uint32_t packed;          /* what those bytes are; see RAM_RAW */

    union {
        char data[RAM_DATA_MAX];
        char entries[RAM_ENTRIES_MAX][RAM_PATH_MAX];
        struct ram_attr attrs[RAM_ATTRS_MAX];
    } u;
};

_Static_assert(sizeof(struct ram_request) <= 2048,
               "a /data request must fit in one message");
_Static_assert(sizeof(struct ram_reply) <= 2048,
               "a /data reply must fit in one message");

#endif /* KOSMOS_RAMPROTO_H */
