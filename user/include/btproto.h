/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_BTPROTO_H
#define KOSMOS_BTPROTO_H

/*
 * **`/Devices/bluetooth`** (`bluetooth.md` B4): what a program may ask the
 * Bluetooth host stack, in the shape both sides compile against -
 * `CLAUDE.md`'s declared struct rather than a table. `bluetooth.lua` is the
 * one place in Lua that knows it.
 *
 *   STATE  the adapter - whether there is one, what it is, its address,
 *          whether it is looking - and how many devices it has heard.
 *   SCAN   look for `arg` milliseconds more, at most `BT_SCAN_MOST`: a
 *          lease, which a program renews while it wants results. When
 *          nobody renews it the stack stops looking by itself, so a window
 *          that closes - or a program that dies - leaves no radio busy
 *          behind it.
 *   STOP   stop looking now.
 *   LIST   a page of the devices heard, from place `arg`, nearest first.
 *
 * **Answered from memory, never from the radio.** What is nearby arrives
 * when it arrives; the stack keeps it, and a request takes what it has
 * (`CLAUDE.md`, *Nothing on the desktop waits on a server*).
 */

#include <stdint.h>

enum {
    BT_OP_STATE = 1,
    BT_OP_SCAN  = 2,
    BT_OP_STOP  = 3,
    BT_OP_LIST  = 4,
};

enum {
    BT_OK           = 0,
    BT_ERR_REQUEST  = 1,    /* not a request this understands */
    BT_ERR_NONE     = 2,    /* there is no adapter */
    BT_ERR_NOT_READY = 3,   /* the adapter is still being set up, or failed */
};

/* What carried the adapter (`hciproto.h`'s HCI_BUS_*). */
enum {
    BT_BUS_USB       = 1,
    BT_BUS_SIMULATED = 2,
};

/* What the adapter can do: `flags` in the state. */
#define BT_CAN_CLASSIC      0x01u
#define BT_CAN_LE           0x02u
#define BT_CAN_SSP          0x04u

/* A device heard: its transport (`gap_decode.h`'s GAP_CLASSIC, LE_PUBLIC,
 * LE_RANDOM) and its kind (GAP_KIND_*), as the stack read them. */
#define BT_SCAN_MOST        30000u  /* ms: the longest lease one SCAN gives */
#define BT_NAME_BYTES       52u
#define BT_LIST_PAGE        24u

struct bt_request {                 /* 16 bytes */
    uint32_t op;
    uint32_t arg;                   /* SCAN: ms; LIST: the first place */
    uint32_t reserved[2];
};

struct bt_device {                  /* 72 bytes */
    uint8_t  address[6];            /* least significant byte first */
    uint8_t  transport;
    uint8_t  kind;
    int8_t   rssi;                  /* dBm, or 127 when it did not say */
    uint8_t  flags;                 /* reserved: paired, connected - B5 */
    uint16_t appearance;            /* LE's, or 0 */
    uint32_t cod;                   /* Classic's Class of Device, or 0 */
    uint32_t heard_ms;              /* how long ago it was last heard */
    char     name[BT_NAME_BYTES];   /* UTF-8, NUL-terminated; "" for none yet */
};

struct bt_reply {                   /* 64 + 24 * 72 = 1792 bytes */
    uint32_t error;
    uint8_t  present;               /* an adapter, set up and ready */
    uint8_t  bus;                   /* BT_BUS_* */
    uint8_t  scanning;              /* looking now */
    uint8_t  flags;                 /* BT_CAN_* */
    uint16_t port;                  /* the USB port, or 0 */
    uint8_t  version;               /* HCI version: 10 is Bluetooth 5.1 */
    uint8_t  reserved;
    uint16_t manufacturer;          /* a company identifier */
    uint16_t acl_length;
    uint16_t acl_packets;
    uint16_t reserved2;
    uint8_t  address[6];            /* the adapter's own */
    uint8_t  reserved3[2];
    uint32_t devices;               /* how many have been heard */
    uint32_t first;                 /* LIST: the place of `list[0]` */
    uint32_t count;                 /* LIST: how many of `list` are filled */
    char     maker[24];             /* the company's name, or "" */
    struct bt_device list[BT_LIST_PAGE];
};

_Static_assert(sizeof(struct bt_request) == 16, "the request is 16 bytes");
_Static_assert(sizeof(struct bt_device) == 72, "a device is 72 bytes");
_Static_assert(sizeof(struct bt_reply) == 64 + BT_LIST_PAGE * 72,
               "the reply is its header and a page of devices");

#endif /* KOSMOS_BTPROTO_H */
