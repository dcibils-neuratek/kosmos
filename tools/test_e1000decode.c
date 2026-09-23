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

    if (fails == 0) {
        printf("PASS: %d checks on an Intel Ethernet controller's registers "
               "and descriptors (the link and its speed, the MAC out of RAL "
               "and RAH, a frame received, one not written back, one that is "
               "not the end of its packet, the errors that matter and the "
               "two that do not).\n", checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on e1000_decode.c.\n", fails, checks);
    return 1;
}
