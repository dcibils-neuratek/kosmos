/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A Bluetooth controller in software. `btsim.h` says what and why; the
 * section numbers are the Core specification's, Volume 4 Part E.
 */

#include "btsim.h"

#include <string.h>

#include "hci_decode.h"
#include "../../include/bytes.h"
#include "../../include/hciring.h"

/* Unknown HCI Command (Volume 1 Part F 1.3). */
#define STATUS_UNKNOWN_COMMAND  0x01u

/* 02:4B:4F:53:4D:4F, least significant byte first as HCI carries it. */
static const uint8_t ADDRESS[6] = { 0x4F, 0x4D, 0x53, 0x4F, 0x4B, 0x02 };

/*
 * Its features (Volume 2 Part C 3.3): what a dual-mode controller of
 * today has, so the stack's choices see what a real one shows - LE (bit
 * 38), Secure Simple Pairing (51), BR/EDR (37 clear), 3-slot and 5-slot
 * packets (0, 1), encryption (2), role switch (5), sniff mode (7).
 */
static const uint8_t FEATURES[8] = {
    0xA7, 0x00, 0x00, 0x00, 0x40, 0x00, 0x08, 0x00,
};

void btsim_start(struct btsim *s)
{
    memset(s, 0, sizeof *s);
}

/* Command Complete for `opcode`: Num_HCI_Command_Packets 1, the status,
 * then `ret`. Queued after whatever is waiting; dropped and counted when
 * there is no room. */
static void complete(struct btsim *s, uint16_t opcode, uint8_t status,
                     const uint8_t *ret, unsigned length)
{
    uint8_t *p;

    if (s->count == BTSIM_QUEUE || length > 250u) {
        s->dropped++;
        return;
    }

    p = s->queue[(s->head + s->count) % BTSIM_QUEUE];
    p[0] = HCI_PACKET_EVENT;
    p[1] = HCI_EVENT_COMMAND_COMPLETE;
    p[2] = (uint8_t)(4u + length);
    p[3] = 1;
    put_le16(p + 4, opcode);
    p[6] = status;

    if (length != 0) {
        memcpy(p + 7, ret, length);
    }

    s->length[(s->head + s->count) % BTSIM_QUEUE] = (uint16_t)(7u + length);
    s->count++;
}

void btsim_take(struct btsim *s, const uint8_t *packet, unsigned length)
{
    uint8_t ret[8];
    uint16_t opcode;

    /* A command is its type, two bytes of opcode, a length, and that many
     * parameters (5.4.1). */
    if (length < 4u || packet[0] != HCI_PACKET_COMMAND
        || length != 4u + packet[3]) {
        return;
    }

    opcode = get_le16(packet + 1);
    s->commands++;

    switch (opcode) {
    case HCI_RESET:
        /* What was waiting is a controller's state, and a reset ends it. */
        s->count = 0;
        s->reset = true;
        complete(s, opcode, 0, 0, 0);
        break;

    case HCI_READ_LOCAL_VERSION:
        ret[0] = BTSIM_VERSION;
        put_le16(ret + 1, 0x0001);
        ret[3] = BTSIM_VERSION;
        put_le16(ret + 4, BTSIM_COMPANY);
        put_le16(ret + 6, 0x0001);
        complete(s, opcode, 0, ret, 8);
        break;

    case HCI_READ_BD_ADDR:
        complete(s, opcode, 0, ADDRESS, 6);
        break;

    case HCI_READ_LOCAL_FEATURES:
        complete(s, opcode, 0, FEATURES, 8);
        break;

    case HCI_READ_BUFFER_SIZE:
        put_le16(ret, BTSIM_ACL_LENGTH);
        ret[2] = 64;
        put_le16(ret + 3, BTSIM_ACL_PACKETS);
        put_le16(ret + 5, 8);
        complete(s, opcode, 0, ret, 7);
        break;

    default:
        complete(s, opcode, STATUS_UNKNOWN_COMMAND, 0, 0);
        break;
    }
}

unsigned btsim_next(struct btsim *s, uint8_t *out, unsigned room)
{
    unsigned n;

    if (s->count == 0) {
        return 0;
    }

    n = s->length[s->head];

    if (n > room) {
        return 0;
    }

    memcpy(out, s->queue[s->head], n);
    s->head = (s->head + 1u) % BTSIM_QUEUE;
    s->count--;

    return n;
}
