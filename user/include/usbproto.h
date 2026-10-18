/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_USBPROTO_H
#define KOSMOS_USBPROTO_H

#include <stdint.h>

/*
 * What you may ask the USB driver about its devices, at `/Devices/usb`
 * (`docs/machine.md` M1): every device on every port of every controller -
 * the ones it drives and the ones it does not - in a declared shape, as
 * `blockproto.h` is. The driver knew all of it and wrote it only to the
 * log; This Machine, `lsusb` and the Deskbar's devices indicator ask here.
 */

#define USB_OP_LIST          1u     /* every device, a page at a time from `first` */

#define USB_OK               0u
#define USB_ERR_BAD_OP       1u

/* The kind the driver made of a device. */
#define USB_KIND_NONE        0u     /* nothing here reads it */
#define USB_KIND_KEYBOARD    1u
#define USB_KIND_MOUSE       2u
#define USB_KIND_GAMEPAD     3u
#define USB_KIND_STICK       4u
#define USB_KIND_NETWORK     5u
#define USB_KIND_CAMERA      6u
#define USB_KIND_MIDI        7u
#define USB_KIND_HID         8u     /* HID, and not one this reads */
#define USB_KIND_STORAGE     9u     /* mass storage this does not speak to */

#define USB_LIST_MAX         16u    /* devices a reply holds */
#define USB_NAME_MAX         32u

struct usb_request {
    uint32_t op;
    uint32_t first;                 /* the index to start from */
};

struct usb_device_info {
    uint8_t  controller;            /* which of the driver's controllers */
    uint8_t  port;                  /* its root hub port, 1 on */
    uint8_t  speed;                 /* 1 full, 2 low, 3 high, 4 super, 5 super+ */
    uint8_t  kind;                  /* USB_KIND_* */
    uint8_t  driven;                /* 1 when the driver reads it */
    uint8_t  class_code, subclass, protocol;   /* the interface it chose, or the first */
    uint16_t vendor, product;
    uint32_t pci;                   /* its controller's PCI place: bus << 8 | slot << 3 | fn */
    char     name[USB_NAME_MAX];    /* the product string it gave, ended */
};

struct usb_reply {
    uint32_t error;
    uint32_t total;                 /* devices there are, all controllers */
    uint32_t count;                 /* of them, in this reply */
    uint32_t reserved;
    struct usb_device_info devices[USB_LIST_MAX];
};

_Static_assert(sizeof(struct usb_device_info) == 48,
               "a USB device is 48 bytes: usb.lua unpacks \"<I1I1I1I1I1I1I1I1I2I2I4c32\"");
_Static_assert(sizeof(struct usb_reply) == 16 + USB_LIST_MAX * 48,
               "the USB reply is four words and sixteen devices");

#endif /* KOSMOS_USBPROTO_H */
