/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A configuration descriptor, walked. `usb_decode.h` says what for.
 *
 * Every layout here is the USB 2.0 specification's, with its table beside it,
 * and the class, subclass and protocol numbers are HID 1.11's.
 *
 * **The walk.** A configuration descriptor is followed by every interface,
 * endpoint and class descriptor of that configuration, one after another,
 * each opening with its own length and type (9.4.3, 9.5). So the walk steps
 * by each descriptor's `bLength` - which is the device's to set. A zero would
 * be a walk that never moves and a large one a walk past the buffer, and both
 * are refused, as is a total longer than what arrived.
 *
 * **Alternate setting 0 only.** It is the setting an interface is in once its
 * configuration is chosen (9.6.5), and choosing another is `SET_INTERFACE`,
 * which nothing here sends - so an endpoint under any other setting is one the
 * device would not be using.
 */

#include <stddef.h>

#include "usb_decode.h"

/* Table 9-5. */
#define DESC_CONFIGURATION  2u
#define DESC_INTERFACE      4u
#define DESC_ENDPOINT       5u

/* Table 9-10. */
#define CONFIG_LENGTH       9u
#define CONFIG_TOTAL        2u          /* wTotalLength, two bytes */
#define CONFIG_VALUE        5u          /* bConfigurationValue */

/* Table 9-12. */
#define IFACE_LENGTH        9u
#define IFACE_NUMBER        2u
#define IFACE_ALTERNATE     3u
#define IFACE_CLASS         5u
#define IFACE_SUBCLASS      6u
#define IFACE_PROTOCOL      7u

/* Table 9-13. */
#define EP_LENGTH           7u
#define EP_ADDRESS          2u          /* 7: IN; 3:0: the number */
#define EP_ATTRIBUTES       3u          /* 1:0: the transfer type */
#define EP_PACKET           4u          /* wMaxPacketSize, two bytes */
#define EP_INTERVAL         6u
#define EP_IN               0x80u
#define EP_NUMBER           0x0Fu
#define EP_INTERRUPT        0x03u

/* HID 1.11, 4.1 to 4.3: the class, the boot subclass, the mouse protocol. */
#define CLASS_HID           3u
#define SUBCLASS_BOOT       1u
#define PROTOCOL_MOUSE      2u

void usb_decode_config(const uint8_t *bytes, unsigned length,
                       struct usb_config *out)
{
    unsigned total, at;
    bool hid = false, in_mouse = false;

    out->kind = USB_CONFIG_MALFORMED;
    out->configuration = 0;
    out->hid_subclass = 0;
    out->hid_protocol = 0;
    out->interface = 0;
    out->endpoint = 0;
    out->packet = 0;
    out->extra = 0;
    out->interval = 0;

    if (bytes == NULL || length < CONFIG_LENGTH
        || bytes[0] < CONFIG_LENGTH || bytes[1] != DESC_CONFIGURATION) {
        return;
    }

    total = bytes[CONFIG_TOTAL] | (unsigned)bytes[CONFIG_TOTAL + 1u] << 8;

    /* What the device said it would send, and no more than arrived. */
    if (total < CONFIG_LENGTH || total > length) {
        return;
    }

    out->configuration = bytes[CONFIG_VALUE];

    for (at = 0; at < total; at += bytes[at]) {
        const uint8_t *d = bytes + at;

        if (total - at < 2u || d[0] < 2u || d[0] > total - at) {
            out->kind = USB_CONFIG_MALFORMED;
            return;
        }

        if (d[1] == DESC_INTERFACE && d[0] >= IFACE_LENGTH) {
            bool is_hid = d[IFACE_CLASS] == CLASS_HID;

            if (is_hid && !hid) {
                hid = true;
                out->hid_subclass = d[IFACE_SUBCLASS];
                out->hid_protocol = d[IFACE_PROTOCOL];
            }

            in_mouse = is_hid && d[IFACE_ALTERNATE] == 0
                    && d[IFACE_SUBCLASS] == SUBCLASS_BOOT
                    && d[IFACE_PROTOCOL] == PROTOCOL_MOUSE;

            if (in_mouse) {
                out->interface = d[IFACE_NUMBER];
            }
        } else if (d[1] == DESC_ENDPOINT && d[0] >= EP_LENGTH && in_mouse) {
            unsigned packet = d[EP_PACKET] | (unsigned)d[EP_PACKET + 1u] << 8;

            if ((d[EP_ADDRESS] & EP_IN) != 0
                && (d[EP_ADDRESS] & EP_NUMBER) != 0
                && (d[EP_ATTRIBUTES] & EP_INTERRUPT) == EP_INTERRUPT
                && (packet & 0x7FFu) != 0) {
                out->kind = USB_CONFIG_BOOT_MOUSE;
                out->endpoint = (uint8_t)(d[EP_ADDRESS] & EP_NUMBER);
                out->packet = (uint16_t)(packet & 0x7FFu);
                out->extra = (uint8_t)((packet >> 11) & 0x3u);
                out->interval = d[EP_INTERVAL];
                return;
            }
        }
    }

    out->kind = hid ? USB_CONFIG_HID_OTHER : USB_CONFIG_NOT_HID;
}
