/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_BLUETOOTH_GAP_DECODE_H
#define KOSMOS_DRIVERS_BLUETOOTH_GAP_DECODE_H

/*
 * **What is nearby, read from what a controller hears** (`bluetooth.md`
 * B4): Classic's inquiry results, Low Energy's advertising reports, a
 * remote name, and what kind of thing each one is - a keyboard, a mouse,
 * headphones.
 *
 * The section numbers are the Core specification's: Volume 4 Part E 7.7 for
 * the events, Volume 3 Part C 8 and the Supplement (CSS) Part A for the
 * data a device says of itself - which Classic calls Extended Inquiry
 * Response and Low Energy advertising data, and which are the same
 * length-type-value form. What a device's class and appearance mean is the
 * Assigned Numbers document's.
 *
 * **Several devices to an event**, laid out one after another, as Linux's
 * `hci_event.c` reads them and as controllers send them: the specification
 * draws Inquiry Result as parallel arrays, and real controllers put each
 * response's fields together. So each decoder calls back once per device.
 *
 * No hardware and no system calls: `tools/test_btdecode.c` holds it on the
 * Mac.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hci_decode.h"

/* Events (Volume 4 Part E 7.7). */
#define HCI_EVENT_INQUIRY_COMPLETE      0x01u
#define HCI_EVENT_INQUIRY_RESULT        0x02u
#define HCI_EVENT_REMOTE_NAME           0x07u
#define HCI_EVENT_INQUIRY_RESULT_RSSI   0x22u
#define HCI_EVENT_EXTENDED_INQUIRY      0x2Fu
#define HCI_EVENT_LE_META               0x3Eu
#define HCI_LE_ADVERTISING_REPORT       0x02u     /* its subevent */

/* What carried a device's news, and so how it is reached. */
#define GAP_CLASSIC      1u     /* BR/EDR: an inquiry result */
#define GAP_LE_PUBLIC    2u     /* Low Energy, a public address */
#define GAP_LE_RANDOM    3u     /* Low Energy, a random one */

/* What a device is, as far as the system cares. */
#define GAP_KIND_OTHER       0u
#define GAP_KIND_KEYBOARD    1u
#define GAP_KIND_MOUSE       2u
#define GAP_KIND_KEYBOARD_MOUSE 3u
#define GAP_KIND_HEADPHONES  4u
#define GAP_KIND_GAMEPAD     5u
#define GAP_KIND_PHONE       6u
#define GAP_KIND_COMPUTER    7u
#define GAP_KIND_INPUT       8u     /* says it is HID, and not which */

/* A name as long as Bluetooth allows is 248 bytes; this keeps 63 and a
 * terminator, cut where a UTF-8 character begins. */
#define GAP_NAME_BYTES   64u

struct gap_found {
    uint8_t  address[6];        /* least significant byte first, as on air */
    uint8_t  transport;         /* GAP_CLASSIC, GAP_LE_* */
    bool     has_rssi;
    int8_t   rssi;              /* dBm: -40 is near, -90 far */
    uint32_t cod;               /* Classic's Class of Device, 24 bits; 0 for LE */
    bool     has_appearance;
    uint16_t appearance;        /* LE's appearance (CSS Part A 1.12) */
    bool     hid;               /* says the HID service, 1812h (LE) */
    bool     connectable;       /* LE: an advertisement it may be connected
                                   to; Classic: always */
    char     name[GAP_NAME_BYTES];   /* "" when it said none */
    bool     name_complete;     /* the Complete Local Name, not shortened */
};

typedef void (*gap_each)(void *ctx, const struct gap_found *found);

/*
 * The length-type-value structures of EIR and advertising data (Volume 3
 * Part C 8): the name, the appearance and whether it offers HID, into `f`,
 * which keeps what it had for anything not said. A structure whose length
 * walks off the end ends the reading there.
 */
void gap_read_data(const uint8_t *data, unsigned length, struct gap_found *f);

/* Inquiry Result, Inquiry Result with RSSI and Extended Inquiry Result:
 * each device found, to `each`. The number found, or 0 for any other event
 * or one too short for what it says it holds. */
unsigned gap_decode_inquiry(const struct hci_event *e, gap_each each,
                            void *ctx);

/* LE Advertising Report (7.7.65.2): each report, to `each`. */
unsigned gap_decode_advertising(const struct hci_event *e, gap_each each,
                                void *ctx);

/* Remote Name Request Complete (7.7.4): whose name, and the name, if the
 * request succeeded. */
bool gap_decode_remote_name(const struct hci_event *e, uint8_t address[6],
                            char name[GAP_NAME_BYTES]);

/* Inquiry Complete (7.7.1): true for the event, its status in `*status`. */
bool gap_decode_inquiry_complete(const struct hci_event *e, uint8_t *status);

/* What it is, from its class or its appearance or its services. */
unsigned gap_kind(const struct gap_found *f);

/* "keyboard", "mouse", "headphones" ... for a line in the log. */
const char *gap_kind_name(unsigned kind);

/* `length` bytes of a name into `out`, which holds `room` with its
 * terminator: cut at a character's start so a UTF-8 character is never
 * halved, and a NUL inside ends it. */
void gap_copy_name(char *out, unsigned room, const uint8_t *name,
                   unsigned length);

#endif
