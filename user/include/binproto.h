/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_BINPROTO_H
#define KOSMOS_BINPROTO_H

#include <stdint.h>

/*
 * What you may ask the /bin server, written down.
 *
 * The third protocol here to be a definition, and the first that has to
 * carry something whose length it does not know: a program's source.
 *
 * **Which raises the rule and does not break it.** `CLAUDE.md` says a stream
 * never travels as a message payload, and the test is the *rate* - if it
 * recurs because the hardware says so, the bytes go in a region. A program's
 * source is not that: it is read once, when somebody launches the thing, and
 * then not again. Nobody's clock is waiting.
 *
 * So this chunks, as the Lua original did, but the chunk is now as large as
 * a message will hold instead of a round kilobyte - which halves the round
 * trips for no work. A twenty-kilobyte program was twenty exchanges and is
 * now eleven.
 *
 * **`read_into` remains the better answer for anything large**, and it
 * already exists: the caller provides a region, the server fills it, and one
 * exchange moves the lot. That is the path a disk file takes. It is not used
 * here because /bin is small, in the image, and read at launch.
 *
 * `data` carries both payloads and `count`/`length` say which: a `list`
 * packs fixed-width names into it, a `read` puts raw source there. One
 * buffer rather than two, because a reply that could hold both at once is a
 * reply with a case nobody has thought about.
 */

#define BIN_OP_LIST     1u
#define BIN_OP_READ     2u
#define BIN_OP_GETATTR  3u

#define BIN_OK              0u
#define BIN_ERR_NO_PROGRAM  1u
#define BIN_ERR_READ_ONLY   2u    /* it is in the image */
#define BIN_ERR_BAD_OP      3u

#define BIN_NAME_MAX    24u       /* a program's name, padded */
#define BIN_WORD_MAX    16u       /* a kind, a section, one `needs` word */
#define BIN_NEEDS_MAX    4u       /* authorities one program may declare */
#define BIN_CHUNK     1792u       /* source bytes, or 74 names, per reply */

struct bin_request {
    uint32_t op;
    uint32_t offset;              /* where in the source, for read */
    char     name[BIN_NAME_MAX];
};

struct bin_reply {
    uint32_t error;
    uint32_t count;               /* names in `data`, for list */
    uint32_t size;                /* the program's whole length */
    uint32_t length;              /* bytes of `data` that are source */
    uint32_t more;                /* 1 when another chunk follows */
    uint32_t windowed;            /* 1 when it draws a window */

    /* Declared in the program's opening comment block and worked out when
     * the store loads, because it is a property of source that cannot
     * change while the system runs - /bin is in the image. */
    char     kind[BIN_WORD_MAX];
    char     section[BIN_WORD_MAX];
    char     needs[BIN_NEEDS_MAX][BIN_WORD_MAX];

    uint8_t  data[BIN_CHUNK];
};

_Static_assert(sizeof(struct bin_reply) <= 2048,
               "a /bin reply must fit in one message - lower BIN_CHUNK");
_Static_assert(BIN_CHUNK / BIN_NAME_MAX >= 64,
               "a list must hold every program in the image");

#endif /* KOSMOS_BINPROTO_H */
