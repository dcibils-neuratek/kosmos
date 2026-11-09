/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **What arrived ahead of the next byte expected** (`net.c`, 9 November
 * 2026), kept so the far end does not send it again.
 *
 * The bytes themselves are not here. They are written straight into the
 * connection's receive ring, at the place they will have when everything
 * before them has come: past `in_write`, in the ring's free space, which
 * only the stack writes and the program never reads until `in_write`
 * passes it. So the ring is the reassembly buffer, and no byte is copied
 * twice. What this keeps is *which* stretches of that space hold bytes
 * yet - up to `TCP_REASM_RANGES` of them, each an offset from the next
 * byte expected (`rcv_nxt`), sorted, never touching another.
 *
 * Overlap needs no policy: TCP's bytes at a sequence number are the bytes
 * at that number, so a piece that arrives twice writes the same bytes
 * twice, and the ranges only grow into a union.
 *
 * When the list is full a piece is not kept, which is what this stack did
 * with every such piece before: correct, and sent again.
 *
 * Header-only and pure, so `tools/test_tcpreasm.c` holds it on the Mac.
 */
#ifndef KOSMOS_TCP_REASM_H
#define KOSMOS_TCP_REASM_H

#include <stdbool.h>
#include <stdint.h>

#define TCP_REASM_RANGES 8u

struct tcp_reasm {
    uint32_t count;
    struct {
        uint32_t from, to;          /* [from, to), offsets from rcv_nxt */
    } range[TCP_REASM_RANGES];
};

static inline void tcp_reasm_clear(struct tcp_reasm *q)
{
    q->count = 0;
}

/*
 * Bytes [from, to) are in the ring now, `from` past the next byte
 * expected. False when there was no room to remember them (they are then
 * as if never received). Merges whatever it overlaps or touches.
 */
static inline bool tcp_reasm_add(struct tcp_reasm *q, uint32_t from, uint32_t to)
{
    uint32_t i = 0, j, k;

    if (from >= to) {
        return true;
    }

    /* The first range that ends at or after `from`: everything before it
     * lies wholly below and is left alone. */
    while (i < q->count && q->range[i].to < from) {
        i++;
    }

    /* Every range from i that starts at or before `to` joins this one. */
    j = i;
    while (j < q->count && q->range[j].from <= to) {
        if (q->range[j].from < from) {
            from = q->range[j].from;
        }
        if (q->range[j].to > to) {
            to = q->range[j].to;
        }
        j++;
    }

    if (j == i) {
        /* Touches nothing: a new range at i, if there is room. */
        if (q->count == TCP_REASM_RANGES) {
            return false;
        }
        for (k = q->count; k > i; k--) {
            q->range[k] = q->range[k - 1];
        }
        q->count++;
    } else {
        /* Ranges i .. j-1 become one, at i. */
        for (k = j; k < q->count; k++) {
            q->range[i + 1 + (k - j)] = q->range[k];
        }
        q->count -= (j - i) - 1;
    }

    q->range[i].from = from;
    q->range[i].to = to;
    return true;
}

/*
 * The next `got` bytes expected have just arrived, in order. Returns how
 * far the bytes now run unbroken from the old next byte expected - `got`,
 * and every kept range that it reaches - and makes the rest's offsets
 * count from the new one.
 */
static inline uint32_t tcp_reasm_advance(struct tcp_reasm *q, uint32_t got)
{
    uint32_t run = got, i = 0, k;

    while (i < q->count && q->range[i].from <= run) {
        if (q->range[i].to > run) {
            run = q->range[i].to;
        }
        i++;
    }

    for (k = i; k < q->count; k++) {
        q->range[k - i].from = q->range[k].from - run;
        q->range[k - i].to = q->range[k].to - run;
    }
    q->count -= i;

    return run;
}

#endif
