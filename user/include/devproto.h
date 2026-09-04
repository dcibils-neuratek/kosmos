/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DEVPROTO_H
#define KOSMOS_DEVPROTO_H

#include <stdint.h>

/*
 * What you may ask the devices server, written down.
 *
 * The second protocol here to be a definition rather than a convention, and
 * it is deliberately not shaped like the first. `audioproto.h` has one
 * struct per operation because the audio server's operations are fixed and
 * few. `/dev` is not like that: it is a description of whatever machine this
 * turned out to be, the nodes differ from one another, and half of what they
 * hold is text meant for a person to read.
 *
 * **So the shape is fixed and the content is not.** A node is at most
 * `DEV_FIELDS` named scalars, each a number or a short string, and nothing
 * nests. That is still "exactly what it expects" in the sense that matters -
 * a caller cannot send a table inside a table, or a megabyte of string, or a
 * field the reader has to guess the type of - while leaving room for a
 * machine to have a clock and a screen or to have neither.
 *
 * Forcing one struct per node would have been the other answer, and it would
 * have meant seven structs of presentation data and a header edit every time
 * a machine grew a peripheral. A protocol should be as strict as the thing
 * it describes, and this thing genuinely varies.
 */

#define DEV_OP_LIST     1u
#define DEV_OP_READ     2u
#define DEV_OP_GETATTR  3u

#define DEV_OK              0u
#define DEV_ERR_NO_KERNEL   1u    /* sysinfo would not answer */
#define DEV_ERR_NO_NODE     2u
#define DEV_ERR_BAD_OP      3u

#define DEV_NAME_MAX    20u       /* a field's name, or a node's */
#define DEV_TEXT_MAX    32u       /* a field's value, when it is words */
#define DEV_FIELDS      24u       /* the most any one node holds */
#define DEV_NODES       12u       /* the most `list` can name */

#define DEV_KIND_NUMBER  0u
#define DEV_KIND_TEXT    1u

struct dev_request {
    uint32_t op;
    char     name[DEV_NAME_MAX];  /* which node, for read */
};

/*
 * Laid out largest first so that the size is what it looks like.
 *
 * 8 + 4 + 20 + 32 is 64 with no padding anywhere, which matters because the
 * Lua side reads this with a `string.pack` format and a compiler that
 * inserted four quiet bytes would put every field after the first one at the
 * wrong offset - visible as a device tree of plausible nonsense rather than
 * as an error.
 */
struct dev_field {
    uint64_t number;
    uint32_t kind;
    char     name[DEV_NAME_MAX];
    char     text[DEV_TEXT_MAX];
};

struct dev_reply {
    uint32_t error;
    uint32_t count;               /* fields, or nodes for a list */
    struct dev_field field[DEV_FIELDS];
};

_Static_assert(sizeof(struct dev_field) == 64,
               "dev_field must be 64 bytes with no padding - the Lua side "
               "reads it with a fixed string.pack format");
_Static_assert(sizeof(struct dev_reply) <= 2048,
               "a devices reply must fit in one message - lower DEV_FIELDS");

#endif /* KOSMOS_DEVPROTO_H */
