/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A USB Bluetooth adapter's configuration, decoded. `btusb_decode.h` says
 * what and why; the section numbers are the Bluetooth Core specification's,
 * Volume 4 Part B.
 */

#include "btusb_decode.h"

#include <string.h>

/* `user/include/bytes.h`, by its path from here: the host test compiles this
 * file with no include path for it. USB is little-endian throughout. */
#include "../../include/bytes.h"

/* Descriptor types (USB 2.0 9.4). */
#define DESC_CONFIGURATION  0x02
#define DESC_INTERFACE      0x04
#define DESC_ENDPOINT       0x05

/* Wireless Controller, RF Controller, Bluetooth Programming (2.1). */
#define CLASS_WIRELESS      0xE0
#define SUBCLASS_RF         0x01
#define PROTOCOL_BLUETOOTH  0x01

/* bmAttributes' transfer type (USB 2.0 9.6.6). */
#define XFER_BULK           2u
#define XFER_INTERRUPT      3u

/*
 * The walk: the first setting 0 of an HCI interface, and its endpoints by
 * type and direction. Once a later interface descriptor arrives the one
 * that was read is over, whether or not it was whole, so a second HCI
 * interface - an adapter with two radios - is not mixed into the first.
 */
void btusb_decode_config(const uint8_t *bytes, unsigned length,
                         struct usb_bluetooth *out)
{
    bool inside = false;            /* in the HCI interface's setting 0 */
    bool done = false;              /* it has been read, and is over */
    unsigned at = 0;

    memset(out, 0, sizeof *out);

    while (at < length) {
        /* A byte left over is a descriptor cut short too, not an end. */
        if (at + 2 > length || bytes[at] < 2 || at + bytes[at] > length) {
            out->malformed = true;
            break;
        }

        unsigned len = bytes[at];
        const uint8_t *d = bytes + at;

        switch (d[1]) {
        case DESC_CONFIGURATION:
            if (len >= 6) out->configuration = d[5];
            break;

        case DESC_INTERFACE:
            if (inside) done = true;
            inside = false;

            if (!done && len >= 9 && d[3] == 0 && d[5] == CLASS_WIRELESS
                && d[6] == SUBCLASS_RF && d[7] == PROTOCOL_BLUETOOTH) {
                inside = true;
                out->interface = d[2];
            }
            break;

        case DESC_ENDPOINT: {
            unsigned number = d[2] & 0x0Fu;
            unsigned type = d[3] & 3u;
            bool in = (d[2] & 0x80u) != 0;

            if (!inside || len < 7 || number == 0) break;

            if (type == XFER_INTERRUPT && in && !out->event_endpoint) {
                out->event_endpoint = (uint8_t)number;
                out->event_packet = get_le16(d + 4) & 0x7FFu;
                out->event_interval = d[6];
            } else if (type == XFER_BULK && in && !out->acl_in) {
                out->acl_in = (uint8_t)number;
                out->acl_in_packet = get_le16(d + 4) & 0x7FFu;
            } else if (type == XFER_BULK && !in && !out->acl_out) {
                out->acl_out = (uint8_t)number;
                out->acl_out_packet = get_le16(d + 4) & 0x7FFu;
            }
            break;
        }

        default:
            break;
        }

        at += len;
    }

    /* A packet size of zero would be a ring that never moves anything. */
    out->ok = !out->malformed && out->event_endpoint && out->acl_in
           && out->acl_out && out->event_packet && out->acl_in_packet
           && out->acl_out_packet;
}
