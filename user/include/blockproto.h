/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_BLOCKPROTO_H
#define KOSMOS_BLOCKPROTO_H

/*
 * **The block protocol**: what a client says to the USB driver about the sticks
 * it holds, and what the driver answers (USB step 5d, `usb.md` §7).
 *
 * `audioproto.h`'s shape, for `audioproto.h`'s reason: the server is C, and the
 * thing on the other side may be wrong, out of date or hostile. Fixed fields,
 * fixed sizes, an error as a number with the sentence composed by whoever
 * shows it to a person - and a request of any other length is refused.
 *
 * **Control by message, data by shared memory.** A request says which blocks.
 * The blocks live in a region the client creates and hands over once, with
 * `BLOCK_OP_OPEN`; the driver reads them through a buffer of its own that the
 * controller reaches, and copies them into that region. A client's pages are
 * never given to the controller (`README.md`, the fifth of step 5's calls).
 *
 * **Two endpoints, one shape** (USB step 5e). `/dev/blocks` is for reading,
 * and every program is given it; a write or a flush that arrives there is
 * refused. The write endpoint takes every operation, and only the disk server
 * is given it. Which endpoint a request came in on is the one thing a server
 * knows about who sent it, so the right to write *is* that endpoint - the
 * kernel's disk grant, pointed at a stick.
 *
 * **A unit is a name** (USB step 5e). A stick is given the next number as it
 * becomes ready and keeps it until it leaves, and no other stick is ever given
 * it - so a unit that has left answers `BLOCK_ERR_NO_UNIT` rather than being
 * some other stick. `BLOCK_OP_INFO` answers, in `count`, how many numbers have
 * been given out, whether or not the unit it asked about is there; a walk of
 * the sticks goes up to it and steps over the gaps.
 */

#include <stdint.h>

#define BLOCK_OP_INFO       1u  /* a unit's size, its block length, its names */
#define BLOCK_OP_OPEN       2u  /* a region capability travels with this one */
#define BLOCK_OP_READ       3u  /* `count` blocks from `lba`, into the region */
#define BLOCK_OP_WRITE      4u  /* `count` blocks at `lba`, from the region */
#define BLOCK_OP_CLOSE      5u  /* the region given back */
#define BLOCK_OP_FLUSH      6u  /* every block the stick caches, written out */

#define BLOCK_OK            0u
#define BLOCK_ERR_BAD_OP    1u  /* no such operation, or a request the wrong size */
#define BLOCK_ERR_NO_UNIT   2u  /* no stick ready at that unit */
#define BLOCK_ERR_NO_REGION 3u  /* no such handle, or not a region big enough */
#define BLOCK_ERR_TOO_MANY  4u  /* more than one read can move, or none */
#define BLOCK_ERR_PAST_END  5u  /* a block past the last */
#define BLOCK_ERR_DEVICE    6u  /* the stick failed it, or did not answer */
#define BLOCK_ERR_READ_ONLY 7u  /* a write or a flush, on the endpoint that reads */
#define BLOCK_ERR_FULL      8u  /* every open slot is taken */
#define BLOCK_ERR_NO_FLUSH  9u  /* a flush, to a stick that has said it does not do one */

/*
 * **The most one read moves: 124 KB.** A read is one Normal TRB, and a Normal
 * TRB's length is seventeen bits - at most 131,071 bytes (xHCI 1.2 6.4.1.1) -
 * so this is the largest whole number of pages under it, which is also a whole
 * number of 512- and 4096-byte blocks. Chaining TRBs would lift it; that waits
 * for a measurement that says it matters.
 */
#define BLOCK_TRANSFER_MOST (31u * 4096u)

struct block_request {
    uint32_t op;
    uint32_t unit;              /* which stick: the number it was given when ready */
    uint64_t lba;               /* the first block */
    uint32_t count;             /* how many blocks */
    uint32_t handle;            /* what `BLOCK_OP_OPEN` answered; 0 otherwise */
};

struct block_reply {
    uint32_t error;             /* BLOCK_OK, or why not */
    uint32_t block_size;        /* bytes in one block of the unit asked about */
    uint64_t blocks;            /* the unit's last block, plus one */
    uint32_t count;             /* blocks moved; for an INFO, units named so far */
    uint32_t handle;            /* for `BLOCK_OP_OPEN`: what reads name */
    char     vendor[8];         /* INQUIRY's, space-padded, as the stick said */
    char     product[16];
};

_Static_assert(sizeof(struct block_request) == 24,
               "a block request is 24 bytes; blocks.lua packs it that way");
_Static_assert(sizeof(struct block_reply) == 48,
               "a block reply is 48 bytes; blocks.lua unpacks it that way");

#endif /* KOSMOS_BLOCKPROTO_H */
