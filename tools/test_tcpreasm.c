/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `user/servers/tcp_reasm.h`, held on the Mac.
 *
 * A stream is sent as pieces in an order of its own - shuffled, some sent
 * twice, some overlapping, some lost and sent again later - into a ring
 * the way `net.c` writes one, and after every piece the kept ranges must
 * be exactly the set a bitmap of the received bytes says, sorted and
 * apart; and at the end the stream read out of the ring must be the
 * stream sent. Then the small cases by hand: touching, swallowing,
 * bridging, and a full list refusing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/servers/tcp_reasm.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static uint32_t seed = 12345;

static uint32_t rnd(uint32_t n)
{
    seed = seed * 1103515245u + 12345u;
    return (seed >> 8) % n;
}

/* The ranges are sorted, apart (never touching), past zero, and inside
 * `limit`. */
static int well_formed(const struct tcp_reasm *q, uint32_t limit)
{
    uint32_t i;

    for (i = 0; i < q->count; i++) {
        if (q->range[i].from == 0 || q->range[i].from >= q->range[i].to
            || q->range[i].to > limit) {
            return 0;
        }
        if (i > 0 && q->range[i - 1].to >= q->range[i].from) {
            return 0;
        }
    }
    return 1;
}

/* Do the ranges cover exactly the marked bytes past `next`? */
static int agrees(const struct tcp_reasm *q, const unsigned char *have,
                  uint32_t next, uint32_t total)
{
    uint32_t at, i;

    for (at = next; at < total; at++) {
        int in = 0;

        for (i = 0; i < q->count; i++) {
            if (at - next >= q->range[i].from && at - next < q->range[i].to) {
                in = 1;
            }
        }
        if (in != (have[at] != 0)) {
            return 0;
        }
    }
    return 1;
}

#define TOTAL  200000u
#define RING   65536u
#define MSS    1460u

static void stream(unsigned trial)
{
    static unsigned char sent[TOTAL], ring[RING], got[TOTAL], have[TOTAL];
    struct tcp_reasm q;
    uint32_t next = 0, read = 0, i, pieces = 0, refused = 0;
    int ok_form = 1, ok_agree = 1;

    for (i = 0; i < TOTAL; i++) {
        sent[i] = (unsigned char)rnd(256);
    }
    memset(have, 0, sizeof have);
    tcp_reasm_clear(&q);

    while (read < TOTAL) {
        uint32_t space, off, seq, len;

        /* The program reads everything published, sometimes - always,
         * once the ring is full, or nothing could ever arrive. */
        if (rnd(3) == 0 || next - read == RING || next == TOTAL) {
            while (read < next) {
                got[read] = ring[read % RING];
                read++;
            }
        }
        if (read == TOTAL) {
            break;
        }

        /* The window: what fits in the ring past what is not read yet. */
        space = RING - (next - read);
        off = rnd(space < 8 * MSS ? space : 8 * MSS);

        if (rnd(4) == 0) {
            off = 0;                    /* the one being waited for */
        }
        seq = next + off;
        if (rnd(10) == 0 && next > 3000) {
            seq = next - rnd(3000);     /* an old one, again */
        }
        len = 1 + rnd(MSS);
        if (seq >= TOTAL) {
            continue;
        }
        if (seq + len > TOTAL) {
            len = TOTAL - seq;
        }
        pieces++;

        /* As net.c takes a segment. */
        if (seq < next) {
            if (seq + len <= next) {
                continue;
            }
            len -= next - seq;
            seq = next;
        }
        off = seq - next;
        if (off >= space) {
            continue;
        }
        if (off + len > space) {
            len = space - off;
        }

        for (i = 0; i < len; i++) {
            ring[(next + off + i) % RING] = sent[seq + i];
        }

        if (off == 0) {
            uint32_t run = tcp_reasm_advance(&q, len);

            for (i = next; i < next + run; i++) {
                have[i] = 0;
            }
            next += run;
        } else if (tcp_reasm_add(&q, off, off + len)) {
            for (i = seq; i < seq + len; i++) {
                have[i] = 1;
            }
        } else {
            refused++;
        }

        ok_form &= well_formed(&q, RING - (next - read));
        if (pieces % 97 == 0) {
            ok_agree &= agrees(&q, have, next, next + RING < TOTAL ? next + RING : TOTAL);
        }
    }

    CHECK(ok_form, "trial %u: the ranges stopped being sorted and apart", trial);
    CHECK(ok_agree, "trial %u: the ranges said other than the bytes received", trial);
    CHECK(memcmp(got, sent, TOTAL) == 0, "trial %u: the stream read back is not the one sent", trial);
    CHECK(refused < pieces, "trial %u: every piece refused", trial);
    if (trial == 0) {
        printf("  %u pieces, %u not kept for want of a range\n", pieces, refused);
    }
}

int main(void)
{
    struct tcp_reasm q;
    unsigned t;

    /* Touching ranges are one. */
    tcp_reasm_clear(&q);
    tcp_reasm_add(&q, 10, 20);
    tcp_reasm_add(&q, 20, 30);
    CHECK(q.count == 1 && q.range[0].from == 10 && q.range[0].to == 30,
          "[10,20) and [20,30) are not one range");

    /* A piece that bridges two swallows both. */
    tcp_reasm_clear(&q);
    tcp_reasm_add(&q, 10, 20);
    tcp_reasm_add(&q, 40, 50);
    tcp_reasm_add(&q, 70, 80);
    tcp_reasm_add(&q, 15, 45);
    CHECK(q.count == 2 && q.range[0].from == 10 && q.range[0].to == 50
          && q.range[1].from == 70, "a bridge did not join [10,20) and [40,50)");

    /* One inside another changes nothing. */
    tcp_reasm_add(&q, 12, 18);
    CHECK(q.count == 2 && q.range[0].to == 50, "a piece inside a range changed it");

    /* In order, up to the first: both come in. */
    CHECK(tcp_reasm_advance(&q, 10) == 50, "ten bytes in order did not reach to 50");
    CHECK(q.count == 1 && q.range[0].from == 20 && q.range[0].to == 30,
          "the range left is not counted from the new next byte");

    /* In order but short of the first: only what came. */
    CHECK(tcp_reasm_advance(&q, 5) == 5 && q.range[0].from == 15,
          "five bytes short of the range took it anyway");

    /* A full list refuses a ninth apart, and takes one that touches. */
    tcp_reasm_clear(&q);
    for (t = 0; t < TCP_REASM_RANGES; t++) {
        tcp_reasm_add(&q, 10 + 20 * t, 20 + 20 * t);
    }
    CHECK(!tcp_reasm_add(&q, 500, 510), "a full list took a ninth range");
    CHECK(tcp_reasm_add(&q, 20, 25) && q.count == TCP_REASM_RANGES,
          "a full list refused a piece that only grew a range");

    for (t = 0; t < 20; t++) {
        stream(t);
    }

    if (failures) {
        printf("FAIL: %d of %d checks on the reassembly ranges\n", failures, checks);
        return 1;
    }
    printf("PASS: %d checks on the reassembly ranges - 20 streams of 200 KB "
           "shuffled, doubled, overlapped and lost, read back whole\n", checks);
    return 0;
}
