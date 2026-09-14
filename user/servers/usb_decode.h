/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERVERS_USB_DECODE_H
#define KOSMOS_SERVERS_USB_DECODE_H

/*
 * What a USB configuration descriptor says, for the xHCI driver: whether the
 * configuration holds a mouse this system can read, and where its reports
 * come from.
 *
 * Its own file, with no hardware and no system calls in it, for the reason
 * `hal/pc/smbios_decode.c` is: the bytes are the device's to choose, QEMU's
 * mouse only ever sends well-formed ones, and refusing a malformed descriptor
 * without walking off the end of the buffer is most of what this is for.
 * `tools/test_usbdecode.c` hands it the ones QEMU cannot.
 */

#include <stdbool.h>
#include <stdint.h>

/* How a configuration's interfaces came out. */
enum usb_config_kind {
    USB_CONFIG_MALFORMED,       /* a length that walks off the end, or none */
    USB_CONFIG_NOT_HID,         /* no interface of the HID class */
    USB_CONFIG_HID_OTHER,       /* HID, and no boot mouse this can read */
    USB_CONFIG_BOOT_MOUSE,      /* a boot mouse, with an interrupt IN endpoint */
};

struct usb_config {
    enum usb_config_kind kind;
    uint8_t  configuration;     /* bConfigurationValue, for SET_CONFIGURATION */

    /* The first HID interface's subclass and protocol, for HID_OTHER's line. */
    uint8_t  hid_subclass;
    uint8_t  hid_protocol;

    /* The boot mouse, when there is one. */
    uint8_t  interface;         /* bInterfaceNumber, for SET_PROTOCOL */
    uint8_t  endpoint;          /* its number, 1 to 15; the direction is IN */
    uint16_t packet;            /* wMaxPacketSize 10:0 */
    uint8_t  extra;             /* wMaxPacketSize 12:11: high speed's more */
    uint8_t  interval;          /* bInterval, as the device said it */

    /* Its HID descriptor's length for the Report descriptor (HID 1.11
     * 6.2.1, wDescriptorLength), which is how much to ask for; 0 if none. */
    uint16_t report_length;
};

/*
 * `bytes` is what GET_DESCRIPTOR returned for the configuration and `length`
 * how many of them arrived. Nothing outside those is read.
 */
void usb_decode_config(const uint8_t *bytes, unsigned length,
                       struct usb_config *out);

/*
 * Where a mouse's buttons and movement are in its reports, out of its Report
 * descriptor (HID 1.11 6.2.2). Bit offsets are counted from the start of the
 * report after its Report ID byte, when it has one. `ok` only for relative X
 * and Y in one report, each 1 to 32 bits: an absolute pair is a tablet.
 */
struct usb_mouse_report {
    bool     ok;
    uint8_t  id;                /* its Report ID; 0 when reports carry none */
    uint8_t  buttons;           /* one-bit Button fields in a row, up to 16 */
    uint16_t buttons_at;        /* the first one's bit */
    uint16_t x_at;
    uint16_t y_at;
    uint8_t  x_bits;
    uint8_t  y_bits;
    bool     x_signed;          /* HID 1.11 5.8: a negative Logical Minimum */
    bool     y_signed;
    uint16_t bits;              /* the whole report's, after the ID */
};

/*
 * `bytes` is the Report descriptor and `length` how many of them there are.
 * Nothing outside those is read. Not reentrant: one caller at a time, which
 * is the driver's one loop.
 */
void usb_decode_mouse_report(const uint8_t *bytes, unsigned length,
                             struct usb_mouse_report *out);

/*
 * `bits` bits of a report from bit `at`, the least significant first, as HID
 * lays fields out - sign-extended when `is_signed`. 0 when `bits` is not 1 to
 * 32 or any of them lies past `length` bytes.
 */
int32_t usb_report_field(const uint8_t *report, unsigned length, unsigned at,
                         unsigned bits, bool is_signed);

#endif /* KOSMOS_SERVERS_USB_DECODE_H */
