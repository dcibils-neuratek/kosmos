/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A Bluetooth controller in software. `btsim.h` says what and why; the
 * section numbers are the Core specification's, Volume 4 Part E.
 */

#include "btsim.h"

#include <string.h>

#include "hci_decode.h"
#include "gap_decode.h"
#include "../../include/bytes.h"
#include "../../include/hciring.h"

/* Unknown HCI Command (Volume 1 Part F 1.3). */
#define STATUS_UNKNOWN_COMMAND  0x01u

/* What it answers beyond the setup (Volume 4 Part E 7). */
#define CMD_INQUIRY             0x0401u
#define CMD_INQUIRY_CANCEL      0x0402u
#define CMD_REMOTE_NAME         0x0419u
#define CMD_SET_EVENT_MASK      0x0C01u
#define CMD_WRITE_INQUIRY_MODE  0x0C45u
#define CMD_LE_SET_EVENT_MASK   0x2001u
#define CMD_LE_SCAN_PARAMETERS  0x200Bu
#define CMD_LE_SCAN_ENABLE      0x200Cu

/*
 * **The neighbours**, made up: addresses that are locally administered
 * (02h first) or random static (the two top bits set), so none is any real
 * device's.
 */
static const uint8_t K380[6]  = { 0x80, 0x03, 0x00, 0x53, 0x4B, 0x02 };
static const uint8_t BUDS[6]  = { 0x5B, 0xD5, 0x00, 0x53, 0x4B, 0x02 };
static const uint8_t MOUSE[6] = { 0x33, 0x58, 0x4D, 0x53, 0x4B, 0xF2 };

/* Peripheral, keyboard, limited discoverable: 00 25 40. */
#define K380_CLASS   0x002540u
/* Audio and Rendering services; Audio/Video, wearable headset. */
#define BUDS_CLASS   0x240404u

/* 02:4B:4F:53:4D:4F, least significant byte first as HCI carries it. */
static const uint8_t ADDRESS[6] = { 0x4F, 0x4D, 0x53, 0x4F, 0x4B, 0x02 };

/*
 * Its features (Volume 2 Part C 3.3): what a dual-mode controller of
 * today has, so the stack's choices see what a real one shows - LE (bit
 * 38), Secure Simple Pairing (51), BR/EDR (37 clear), 3-slot and 5-slot
 * packets (0, 1), encryption (2), role switch (5), sniff mode (7),
 * and Extended Inquiry Response (48).
 */
static const uint8_t FEATURES[8] = {
    0xA7, 0x00, 0x00, 0x00, 0x40, 0x00, 0x09, 0x00,
};

void btsim_start(struct btsim *s)
{
    memset(s, 0, sizeof *s);
}

/* An event, queued after whatever is waiting; dropped and counted when
 * there is no room. */
static void event(struct btsim *s, uint8_t code, const uint8_t *params,
                  unsigned length)
{
    uint8_t *p;

    if (s->count == BTSIM_QUEUE || 3u + length > BTSIM_PACKET_MAX
        || length > 255u) {
        s->dropped++;
        return;
    }

    p = s->queue[(s->head + s->count) % BTSIM_QUEUE];
    p[0] = HCI_PACKET_EVENT;
    p[1] = code;
    p[2] = (uint8_t)length;

    if (length != 0) {
        memcpy(p + 3, params, length);
    }

    s->length[(s->head + s->count) % BTSIM_QUEUE] = (uint16_t)(3u + length);
    s->count++;
}

/* Command Complete for `opcode`: Num_HCI_Command_Packets 1, the status,
 * then `ret`. */
static void complete(struct btsim *s, uint16_t opcode, uint8_t status,
                     const uint8_t *ret, unsigned length)
{
    uint8_t p[255];

    if (length > 251u) {
        s->dropped++;
        return;
    }

    p[0] = 1;
    put_le16(p + 1, opcode);
    p[3] = status;

    if (length != 0) {
        memcpy(p + 4, ret, length);
    }

    event(s, HCI_EVENT_COMMAND_COMPLETE, p, 4u + length);
}

/* Command Status: the command taken, its result to follow (7.7.15). */
static void taken(struct btsim *s, uint16_t opcode, uint8_t result)
{
    uint8_t p[4];

    p[0] = result;
    p[1] = 1;
    put_le16(p + 2, opcode);
    event(s, HCI_EVENT_COMMAND_STATUS, p, 4);
}

/* One Classic neighbour, as an Extended Inquiry Result (7.7.38): its
 * address, its class, RSSI, and 240 bytes of EIR - a name, when it gives
 * one there. */
static void found_classic(struct btsim *s, const uint8_t address[6],
                          uint32_t class_of_device, int8_t rssi,
                          const char *name)
{
    uint8_t p[255];
    unsigned n = name != 0 ? (unsigned)strlen(name) : 0u;

    memset(p, 0, sizeof p);
    p[0] = 1;
    memcpy(p + 1, address, 6);
    p[7] = 1;                               /* page scan repetition R1 */
    p[9] = (uint8_t)class_of_device;
    p[10] = (uint8_t)(class_of_device >> 8);
    p[11] = (uint8_t)(class_of_device >> 16);
    p[14] = (uint8_t)rssi;

    if (n != 0) {
        p[15] = (uint8_t)(1u + n);
        p[16] = 0x09;                       /* Complete Local Name */
        memcpy(p + 17, name, n);
    }

    event(s, HCI_EVENT_EXTENDED_INQUIRY, p, 255);
}

/* The mouse's advertisement (7.7.65.2): connectable and undirected, a
 * random address, and its data - flags, appearance Mouse (03C2h), the HID
 * service, its name. */
static void found_mouse(struct btsim *s)
{
    static const uint8_t data[] = {
        2, 0x01, 0x06,
        3, 0x19, 0xC2, 0x03,
        3, 0x03, 0x12, 0x18,
        14, 0x09, 'M', 'X', ' ', 'A', 'n', 'y', 'w', 'h', 'e', 'r', 'e',
        ' ', '3',
    };
    uint8_t p[64];
    unsigned at = 0;

    p[at++] = HCI_LE_ADVERTISING_REPORT;
    p[at++] = 1;
    p[at++] = 0x00;                         /* ADV_IND */
    p[at++] = 0x01;                         /* a random address */
    memcpy(p + at, MOUSE, 6);
    at += 6;
    p[at++] = (uint8_t)sizeof data;
    memcpy(p + at, data, sizeof data);
    at += sizeof data;
    p[at++] = (uint8_t)(int8_t)-60;

    event(s, HCI_EVENT_LE_META, p, at);
}

/* Remote Name Request Complete (7.7.4) for whichever neighbour was asked:
 * the headphones answer "Pixel Buds", and an address that is not one of
 * them answers Page Timeout, as a device gone away would. */
static void remote_name(struct btsim *s, const uint8_t address[6])
{
    uint8_t p[255];
    const char *name = 0;

    if (memcmp(address, BUDS, 6) == 0) {
        name = "Pixel Buds";
    } else if (memcmp(address, K380, 6) == 0) {
        name = "Keyboard K380";
    }

    memset(p, 0, sizeof p);
    p[0] = name != 0 ? 0x00 : 0x04;         /* success, or Page Timeout */
    memcpy(p + 1, address, 6);

    if (name != 0) {
        memcpy(p + 7, name, strlen(name));
    }

    event(s, HCI_EVENT_REMOTE_NAME, p, 255);
}

void btsim_take(struct btsim *s, const uint8_t *packet, unsigned length)
{
    uint8_t ret[8];
    uint16_t opcode;
    const uint8_t *params = packet + 4;

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

    case CMD_SET_EVENT_MASK:
    case CMD_WRITE_INQUIRY_MODE:
    case CMD_LE_SET_EVENT_MASK:
    case CMD_LE_SCAN_PARAMETERS:
        complete(s, opcode, 0, 0, 0);
        break;

    /* Inquiry (7.1.1): taken, then what it finds, then complete. A real
     * one takes seconds; this one's results are all there at once. */
    case CMD_INQUIRY:
        s->inquiries++;
        taken(s, opcode, 0);
        found_classic(s, K380, K380_CLASS, -48, "Keyboard K380");
        found_classic(s, BUDS, BUDS_CLASS, -71, 0);
        ret[0] = 0;
        event(s, HCI_EVENT_INQUIRY_COMPLETE, ret, 1);
        break;

    /* Nothing is in progress by the time a cancel comes: done. */
    case CMD_INQUIRY_CANCEL:
        complete(s, opcode, 0, 0, 0);
        break;

    /* LE Set Scan Enable (7.8.11): on, then the mouse heard once. */
    case CMD_LE_SCAN_ENABLE:
        complete(s, opcode, 0, 0, 0);

        if (packet[3] >= 1u && params[0] == 1u && !s->scanning) {
            s->scanning = true;
            found_mouse(s);
        } else if (packet[3] >= 1u && params[0] == 0u) {
            s->scanning = false;
        }
        break;

    /* Remote Name Request (7.1.19): taken, then the name. */
    case CMD_REMOTE_NAME:
        if (packet[3] < 10u) {
            complete(s, opcode, STATUS_UNKNOWN_COMMAND, 0, 0);
            break;
        }
        s->names_asked++;
        taken(s, opcode, 0);
        remote_name(s, params);
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
