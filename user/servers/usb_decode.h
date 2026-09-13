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
};

/*
 * `bytes` is what GET_DESCRIPTOR returned for the configuration and `length`
 * how many of them arrived. Nothing outside those is read.
 */
void usb_decode_config(const uint8_t *bytes, unsigned length,
                       struct usb_config *out);

#endif /* KOSMOS_SERVERS_USB_DECODE_H */
