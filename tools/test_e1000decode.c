/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What an Intel Ethernet controller's registers and descriptors say, on the
 * host - `user/servers/e1000_decode.c`.
 *
 * The card cannot be asked for a descriptor with a sequence error in it, or
 * for a link at a speed the specification reserves. This can.
 *
 * Usage: test_e1000decode
 */

#include <stdio.h>
#include <string.h>

#include "e1000_decode.h"

static int checks, fails;

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        fails++;
        printf("  %s\n", what);
    }
}

/* A legacy receive descriptor, as the card writes one back. */
static void rx_desc(uint8_t *d, unsigned length, uint8_t status, uint8_t err)
{
    memset(d, 0, E1000_DESC_BYTES);
    d[8] = (uint8_t)(length & 0xFFu);
    d[9] = (uint8_t)(length >> 8);
    d[12] = status;
    d[13] = err;
}

int main(void)
{
    struct e1000_link link;
    struct e1000_rx rx;
    uint8_t desc[E1000_DESC_BYTES];
    uint8_t mac[6];

    /*
     * The link. STATUS bit 1 is up, bit 0 is full duplex, and bits 7:6 are
     * the speed - 00 ten, 01 a hundred, 10 a thousand, 11 reserved.
     */
    e1000_decode_link(0x00000083u, &link);      /* up, full, 1000 */
    check(link.up && link.full_duplex && link.mbps == 1000,
          "a gigabit link at full duplex did not read as one");

    e1000_decode_link(0x00000042u, &link);      /* up, half, 100 */
    check(link.up && !link.full_duplex && link.mbps == 100,
          "a hundred-megabit link at half duplex did not read as one");

    e1000_decode_link(0x00000002u, &link);
    check(link.up && link.mbps == 10, "a ten-megabit link did not read as one");

    e1000_decode_link(0x00000000u, &link);
    check(!link.up, "a link that is down read as up");

    e1000_decode_link(0x000000C2u, &link);      /* 11b: reserved */
    check(link.up && link.mbps == 0,
          "a reserved speed gave a number. The link is still up - those are "
          "two facts and a card at a speed this does not know is still a "
          "card that is connected");

    /*
     * The MAC, out of the Receive Address registers the firmware leaves
     * programmed. 00:1f:c6:9c:a8:2b as the card holds it.
     */
    check(e1000_decode_mac(0x9CC61F00u, 0x80002BA8u, mac)
          && mac[0] == 0x00 && mac[1] == 0x1f && mac[2] == 0xc6
          && mac[3] == 0x9c && mac[4] == 0xa8 && mac[5] == 0x2b,
          "the MAC did not come out of RAL and RAH in the order the card "
          "holds it: RAL's first byte is the address's first");

    memset(mac, 0x5a, sizeof(mac));
    check(!e1000_decode_mac(0x9CC61F00u, 0x00002BA8u, mac)
          && mac[0] == 0x5a,
          "an address with RAH's valid bit clear was taken, and the answer "
          "changed");

    check(!e1000_decode_mac(0u, 0x80000000u, mac),
          "00:00:00:00:00:00 with the valid bit set was taken for an "
          "address; nothing answers to it and every switch refuses it");

    check(!e1000_decode_mac(0x9CC61F00u, 0x80002BA8u, NULL),
          "no room for an address was not refused");

    /* A receive descriptor: done, the end of its packet, no errors. */
    rx_desc(desc, 1514u, 0x03u, 0x00u);
    e1000_decode_rx(desc, &rx);
    check(rx.done && rx.end && !rx.error && rx.length == 1514,
          "a finished descriptor of 1514 bytes did not read as one");

    /* Not written back yet: nothing else in it means anything. */
    rx_desc(desc, 1514u, 0x00u, 0x00u);
    e1000_decode_rx(desc, &rx);
    check(!rx.done && rx.length == 0,
          "a descriptor the card has not written back was read, and its "
          "length believed");

    /* Done and not the end: a frame longer than one buffer. */
    rx_desc(desc, 2048u, 0x01u, 0x00u);
    e1000_decode_rx(desc, &rx);
    check(rx.done && !rx.end,
          "a descriptor that is not the end of its packet read as one");

    /* A CRC error. */
    rx_desc(desc, 1514u, 0x03u, 0x01u);
    e1000_decode_rx(desc, &rx);
    check(rx.done && rx.error, "a CRC error was not reported");

    /* A sequence error, and a symbol error. */
    rx_desc(desc, 64u, 0x03u, 0x04u);
    e1000_decode_rx(desc, &rx);
    check(rx.error, "a sequence error was not reported");

    rx_desc(desc, 64u, 0x03u, 0x02u);
    e1000_decode_rx(desc, &rx);
    check(rx.error, "a symbol error was not reported");

    /*
     * **And the two that are not errors here.** TCPE and IPE say the card
     * did not verify a checksum, and this driver never asked it to and does
     * not read the result: a frame marked with them is a frame that arrived.
     */
    rx_desc(desc, 1514u, 0x03u, 0x20u);
    e1000_decode_rx(desc, &rx);
    check(!rx.error, "a TCP checksum bit was read as a frame in error, and "
                     "this driver does not ask the card to check one");

    rx_desc(desc, 1514u, 0x03u, 0x40u);
    e1000_decode_rx(desc, &rx);
    check(!rx.error, "an IP checksum bit was read as a frame in error");

    e1000_decode_rx(NULL, &rx);
    check(!rx.done, "no descriptor at all was read");

    /* A transmit descriptor's own done bit. */
    memset(desc, 0, sizeof(desc));
    check(!e1000_decode_tx_done(desc), "an unfinished transmit read as done");
    desc[12] = 0x01u;
    check(e1000_decode_tx_done(desc), "a finished transmit did not read so");
    check(!e1000_decode_tx_done(NULL), "no transmit descriptor read as done");

    /*
     * **A receive descriptor in the extended layout** (`testing.md` 18.349):
     * `e1000e`'s `union e1000_rx_desc_extended` - the status word at byte 8,
     * the length at byte 12. A 1514-byte frame, done and the end of its
     * packet, as the I219 writes it with RFCTL.EXTEN on.
     */
    {
        uint8_t ext[E1000_DESC_BYTES];
        struct e1000_rx legacy;

        memset(ext, 0, sizeof(ext));
        ext[8] = 0x03u;                     /* DD and EOP */
        ext[12] = (uint8_t)(1514u & 0xFFu);
        ext[13] = (uint8_t)(1514u >> 8);

        e1000_decode_rx_ext(ext, &rx);
        check(rx.done && rx.end && !rx.error && rx.length == 1514,
              "an extended descriptor's frame was not read as done, whole and "
              "1514 bytes");

        /* The same bytes read as legacy are the bug this decoder is for:
         * the length's low byte, 0xEA, taken for a status - not done. */
        e1000_decode_rx(ext, &legacy);
        check(!legacy.done,
              "an extended descriptor read as legacy came out done, so the "
              "test cannot tell the two layouts apart");

        ext[11] = 0x80u;                    /* RXE, the top error bit */
        e1000_decode_rx_ext(ext, &rx);
        check(rx.done && rx.error, "an extended descriptor's RX error was not "
                                   "reported");

        ext[11] = 0x01u;                    /* CE, a CRC error */
        e1000_decode_rx_ext(ext, &rx);
        check(rx.error, "an extended descriptor's CRC error was not reported");

        ext[11] = 0x08u;                    /* bit 27, not a frame error */
        e1000_decode_rx_ext(ext, &rx);
        check(!rx.error, "a bit outside e1000e's frame errors was read as one");

        memset(ext, 0, sizeof(ext));
        e1000_decode_rx_ext(ext, &rx);
        check(!rx.done && rx.length == 0,
              "an extended descriptor not written back read as done");
    }

    /*
     * **The transmit ring, kept without waiting** (`testing.md` 18.345). The
     * driver waited for every frame's write-back, so a card that never wrote
     * one - the M700's I219 - held the driver, the network stack behind it,
     * and every program asking the stack anything. Now the confirmations are
     * collected afterwards, and this is the arithmetic that has to be right
     * for that: never a descriptor the card still has, every one it finished.
     */
    {
        uint8_t ring[8 * E1000_DESC_BYTES];
        struct e1000_tx_ring r = { 8u, 0u, 0u };
        unsigned i, written = 0;

        memset(ring, 0, sizeof(ring));

        check(e1000_tx_room(&r) && e1000_tx_out(&r) == 0,
              "an empty ring had no room, or said frames were out");
        check(e1000_tx_reclaim(&r, ring) == 0 && r.clean == 0,
              "an empty ring gave back a confirmation nobody was owed");

        /* A card that confirms nothing: seven go out and the eighth is
         * refused, because one descriptor is always left unwritten. */
        while (e1000_tx_room(&r)) {
            r.next = (r.next + 1u) % r.slots;
            written++;
        }

        check(written == 7 && e1000_tx_out(&r) == 7,
              "a ring of eight that the card never confirmed took a number "
              "of frames other than seven before it had no room");
        check(e1000_tx_reclaim(&r, ring) == 0 && !e1000_tx_room(&r),
              "a ring the card confirmed none of was given room back");

        /* The card finishes the first three, in order: three back, and
         * room again. */
        for (i = 0; i < 3u; i++) {
            ring[i * E1000_DESC_BYTES + 12u] = 0x01u;
        }

        check(e1000_tx_reclaim(&r, ring) == 3 && r.clean == 3
              && e1000_tx_out(&r) == 4 && e1000_tx_room(&r),
              "three confirmations were not collected as three, with room "
              "after them");

        /* A done bit past one that is not done is not taken: the card
         * finishes in order, and skipping a hole would hand a descriptor it
         * still holds back to the writer. */
        ring[5u * E1000_DESC_BYTES + 12u] = 0x01u;
        check(e1000_tx_reclaim(&r, ring) == 0 && r.clean == 3,
              "a confirmation past an unconfirmed frame was collected");

        /* And the rest, across the end of the ring and round to the start. */
        ring[3u * E1000_DESC_BYTES + 12u] = 0x01u;
        ring[4u * E1000_DESC_BYTES + 12u] = 0x01u;
        ring[6u * E1000_DESC_BYTES + 12u] = 0x01u;
        check(e1000_tx_reclaim(&r, ring) == 4 && r.clean == r.next
              && e1000_tx_out(&r) == 0,
              "the last four were not collected, or the ring did not read as "
              "empty after them");

        memset(ring, 0, sizeof(ring));
        r.clean = 6u;                   /* 6, 7 and 0 out: across the end */
        r.next = 1u;
        ring[6u * E1000_DESC_BYTES + 12u] = 0x01u;
        ring[7u * E1000_DESC_BYTES + 12u] = 0x01u;
        ring[0u * E1000_DESC_BYTES + 12u] = 0x01u;
        check(e1000_tx_out(&r) == 3 && e1000_tx_reclaim(&r, ring) == 3
              && r.clean == 1u,
              "frames out across the end of the ring were not counted and "
              "collected as three");

        check(!e1000_tx_room(NULL) && e1000_tx_out(NULL) == 0
              && e1000_tx_reclaim(NULL, ring) == 0,
              "no ring at all read as one with room or frames out");
    }

    if (fails == 0) {
        printf("PASS: %d checks on an Intel Ethernet controller's registers "
               "and descriptors (the link and its speed, the MAC out of RAL "
               "and RAH, a frame received, one not written back, one that is "
               "not the end of its packet, the errors that matter and the "
               "two that do not, and the transmit ring kept without "
               "waiting).\n", checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on e1000_decode.c.\n", fails, checks);
    return 1;
}
