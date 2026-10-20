/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_USB_BTUSB_DECODE_H
#define KOSMOS_DRIVERS_USB_BTUSB_DECODE_H

/*
 * **A USB Bluetooth adapter, as its configuration describes it**
 * (`bluetooth.md` B1): which interface carries HCI, and the three endpoints
 * it is spoken to through - events on an interrupt IN, ACL data on a bulk
 * IN and a bulk OUT. Commands go on the control endpoint and need nothing
 * from here.
 *
 * The layout is the Bluetooth Core specification's, Volume 4 Part B 2.1:
 * interface 0, setting 0, of class E0h (Wireless Controller), subclass 01h
 * (RF Controller), protocol 01h (Bluetooth Programming), with those three
 * endpoints; then interface 1, whose settings carry voice on isochronous
 * endpoints, which a keyboard and a mouse do not need and this does not
 * read.
 *
 * Its own file with no hardware and no system calls in it, for the reason
 * `midi_decode.c` is: the bytes are the device's to choose and QEMU has no
 * Bluetooth adapter, so `tools/test_btdecode.c` holds this to the layout
 * the specification gives and to the malformed bytes no real adapter sends.
 *
 * Not read: an adapter that calls itself vendor-specific (FFh) and is
 * Bluetooth anyway, which Linux's `btusb.c` knows by vendor and product.
 * One arrives with the adapter that needs it.
 */

#include <stdbool.h>
#include <stdint.h>

struct usb_bluetooth {
    bool     ok;                /* an HCI interface with all three endpoints */
    uint8_t  configuration;     /* bConfigurationValue */
    uint8_t  interface;         /* bInterfaceNumber, setting 0 */

    uint8_t  event_endpoint;    /* interrupt IN, 1 to 15: HCI events */
    uint16_t event_packet;      /* its wMaxPacketSize, 10:0 */
    uint8_t  event_interval;    /* bInterval, as the adapter said it */

    uint8_t  acl_in;            /* bulk IN: ACL data from the controller */
    uint16_t acl_in_packet;
    uint8_t  acl_out;           /* bulk OUT: ACL data to it */
    uint16_t acl_out_packet;

    bool     malformed;         /* a length that walks off the end, or zero */
};

/* Nothing outside `bytes[0 .. length)` is read. */
void btusb_decode_config(const uint8_t *bytes, unsigned length,
                         struct usb_bluetooth *out);

#endif
