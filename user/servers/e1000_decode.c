/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What an Intel Ethernet controller says about itself. See the header.
 */

#include <string.h>

#include "e1000_decode.h"

/* STATUS, and the two bits that carry the speed (13.4.2). */
#define STATUS_FD           (1u << 0)
#define STATUS_LU           (1u << 1)
#define STATUS_SPEED_SHIFT  6u
#define STATUS_SPEED_MASK   0x3u

/* RAH's Address Valid (13.4.32). */
#define RAH_AV              (1u << 31)

/* A receive descriptor's status and error bytes (3.2.3). */
#define RX_STATUS_AT        12u
#define RX_ERRORS_AT        13u
#define RX_LENGTH_AT        8u
#define RX_STATUS_DD        (1u << 0)
#define RX_STATUS_EOP       (1u << 1)

/*
 * **CE, SE, SEQ, CXE and RXE** - the errors that mean the bytes are wrong,
 * named one at a time rather than as everything-but, because the two left
 * out are left out deliberately: TCPE (5) and IPE (6) are checksum-offload
 * results, and they say the *card* did not verify a checksum this driver
 * never asked it to and does not read. Bit 3 is reserved.
 */
#define RX_ERROR_CE         (1u << 0)   /* CRC, or an alignment error */
#define RX_ERROR_SE         (1u << 1)   /* a symbol error */
#define RX_ERROR_SEQ        (1u << 2)   /* a sequence error */
#define RX_ERROR_CXE        (1u << 4)   /* carrier extension */
#define RX_ERROR_RXE        (1u << 7)   /* the card's own data error */
#define RX_ERROR_BAD        (RX_ERROR_CE | RX_ERROR_SE | RX_ERROR_SEQ \
                             | RX_ERROR_CXE | RX_ERROR_RXE)

/* A transmit descriptor's status byte (3.3.3). */
#define TX_STATUS_AT        12u
#define TX_STATUS_DD        (1u << 0)

void e1000_decode_link(uint32_t status, struct e1000_link *out)
{
    static const unsigned speeds[4] = { 10u, 100u, 1000u, 0u };

    if (out == NULL) {
        return;
    }

    out->up = (status & STATUS_LU) != 0;
    out->full_duplex = (status & STATUS_FD) != 0;
    out->mbps = speeds[(status >> STATUS_SPEED_SHIFT) & STATUS_SPEED_MASK];
}

bool e1000_decode_mac(uint32_t ral, uint32_t rah, uint8_t mac[6])
{
    uint8_t got[6];

    if (mac == NULL || (rah & RAH_AV) == 0) {
        return false;
    }

    got[0] = (uint8_t)(ral & 0xFFu);
    got[1] = (uint8_t)((ral >> 8) & 0xFFu);
    got[2] = (uint8_t)((ral >> 16) & 0xFFu);
    got[3] = (uint8_t)((ral >> 24) & 0xFFu);
    got[4] = (uint8_t)(rah & 0xFFu);
    got[5] = (uint8_t)((rah >> 8) & 0xFFu);

    /*
     * All zeros is not an address, and a card whose registers were never
     * written reads as zeros with the valid bit set on some firmware. An
     * interface with 00:00:00:00:00:00 answers nothing and is refused by
     * every switch, so it is better called absent here than carried.
     */
    if (got[0] == 0 && got[1] == 0 && got[2] == 0
        && got[3] == 0 && got[4] == 0 && got[5] == 0) {
        return false;
    }

    memcpy(mac, got, sizeof(got));
    return true;
}

void e1000_decode_rx(const uint8_t *desc, struct e1000_rx *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    if (desc == NULL) {
        return;
    }

    if ((desc[RX_STATUS_AT] & RX_STATUS_DD) == 0) {
        return;
    }

    out->done = true;
    out->end = (desc[RX_STATUS_AT] & RX_STATUS_EOP) != 0;
    out->error = (desc[RX_ERRORS_AT] & RX_ERROR_BAD) != 0;
    out->length = (unsigned)desc[RX_LENGTH_AT]
                | ((unsigned)desc[RX_LENGTH_AT + 1u] << 8);
}

bool e1000_decode_tx_done(const uint8_t *desc)
{
    return desc != NULL && (desc[TX_STATUS_AT] & TX_STATUS_DD) != 0;
}
