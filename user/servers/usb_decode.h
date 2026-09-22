/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERVERS_USB_DECODE_H
#define KOSMOS_SERVERS_USB_DECODE_H

/*
 * What a USB configuration descriptor says, for the xHCI driver: whether the
 * configuration holds a mouse this system can read, and where its reports
 * come from - or a stick, and the two bulk endpoints it is spoken to through,
 * or an Ethernet adapter, and which of its interfaces carries the frames.
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
    USB_CONFIG_NEITHER,         /* no HID interface and no mass storage one */
    USB_CONFIG_HID_OTHER,       /* HID, and no boot mouse this can read */
    USB_CONFIG_BOOT_MOUSE,      /* a boot mouse, with an interrupt IN endpoint */
    USB_CONFIG_STORAGE_OTHER,   /* mass storage, and no stick this can speak to */
    USB_CONFIG_BULK_ONLY,       /* SCSI over Bulk-Only, a bulk IN and a bulk OUT */
    USB_CONFIG_XBOX360,         /* an Xbox 360 controller's interface, FFh/5Dh/01h,
                                   with an interrupt IN endpoint */
    USB_CONFIG_XBOXONE,         /* an Xbox One or Series controller's, FFh/47h/D0h,
                                   with an interrupt IN and an interrupt OUT */
};

struct usb_config {
    enum usb_config_kind kind;
    uint8_t  configuration;     /* bConfigurationValue, for SET_CONFIGURATION */

    /* The first HID interface's subclass and protocol, for HID_OTHER's line. */
    uint8_t  hid_subclass;
    uint8_t  hid_protocol;

    /* The boot mouse, when there is one - or the Xbox 360 controller's
     * interface, whose endpoint is read the same way. */
    uint8_t  interface;         /* bInterfaceNumber, for SET_PROTOCOL */
    uint8_t  endpoint;          /* its number, 1 to 15; the direction is IN */
    uint16_t packet;            /* wMaxPacketSize 10:0 */
    uint8_t  extra;             /* wMaxPacketSize 12:11: high speed's more */
    uint8_t  interval;          /* bInterval, as the device said it */

    /* Its HID descriptor's length for the Report descriptor (HID 1.11
     * 6.2.1, wDescriptorLength), which is how much to ask for; 0 if none. */
    uint16_t report_length;

    /* An Xbox One pad's interrupt OUT, which it has to be spoken to on
     * before it reports anything (`pad_decode.h`); all zero otherwise. */
    uint8_t  out_endpoint;
    uint16_t out_packet;
    uint8_t  out_interval;

    /* The first mass storage interface's subclass and protocol, for
     * STORAGE_OTHER's line. */
    uint8_t  storage_subclass;
    uint8_t  storage_protocol;

    /* The stick, when there is one: its interface, and each bulk endpoint's
     * number, largest packet, and burst - the SuperSpeed Endpoint Companion's
     * bMaxBurst, 0 to 15, and 0 without one. All zero unless BULK_ONLY. */
    uint8_t  storage_interface;
    uint8_t  bulk_in;
    uint8_t  bulk_out;
    uint16_t bulk_in_packet;
    uint16_t bulk_out_packet;
    uint8_t  bulk_in_burst;
    uint8_t  bulk_out_burst;

    /* The first interface's class, subclass and protocol, and how many
     * interface descriptors there were - alternate settings included - for
     * the line that says what a device is when nothing here reads it. */
    uint8_t  first_class;
    uint8_t  first_subclass;
    uint8_t  first_protocol;
    uint8_t  interfaces;
};

/*
 * `bytes` is what GET_DESCRIPTOR returned for the configuration and `length`
 * how many of them arrived. Nothing outside those is read.
 */
void usb_decode_config(const uint8_t *bytes, unsigned length,
                       struct usb_config *out);

/*
 * **A USB Ethernet adapter**: a configuration's CDC-ECM function, if it has
 * one (`usb_decode.c` has whose layouts). Which configuration, its two
 * interfaces, which setting of the Data interface carries the frames - not
 * setting 0, which has no endpoints - and its endpoints. All zero unless
 * `ok`.
 */
struct usb_ecm {
    bool     ok;
    uint8_t  configuration;     /* bConfigurationValue */
    uint8_t  control;           /* the Communications interface */
    uint8_t  data;              /* the Data interface, by its Union */
    uint8_t  data_alternate;    /* its setting with the two bulk endpoints */
    uint8_t  mac_string;        /* iMACAddress: a string descriptor's index */
    uint16_t max_segment;       /* wMaxSegmentSize: the largest frame */

    /* Its notifications - link up, link down, the speed - on an interrupt
     * IN; all zero when there is none. */
    uint8_t  notify;
    uint16_t notify_packet;
    uint8_t  notify_interval;

    /* The frames, a bulk endpoint each way, as a stick's are. */
    uint8_t  bulk_in;
    uint8_t  bulk_out;
    uint16_t bulk_in_packet;
    uint16_t bulk_out_packet;
    uint8_t  bulk_in_burst;
    uint8_t  bulk_out_burst;
};

/* Nothing outside `bytes[0 .. length)` is read. */
void usb_decode_ecm(const uint8_t *bytes, unsigned length,
                    struct usb_ecm *out);

/*
 * The six bytes of a MAC address out of the string descriptor ECM 5.4 keeps
 * it in; false, and `mac` untouched, for anything that is not exactly twelve
 * hex digits.
 */
bool usb_decode_mac(const uint8_t *desc, unsigned length, uint8_t mac[6]);

/*
 * **What an adapter says on its interrupt endpoint** (CDC 1.2 6.3): an
 * eight-byte header shaped like a Setup packet the other way round -
 * bmRequestType A1h, bNotificationCode, wValue, wIndex, wLength - and
 * `wLength` bytes after it.
 *
 * Only the two an Ethernet function sends are read. NETWORK_CONNECTION
 * (6.3.1) carries the link in wValue and nothing after it;
 * CONNECTION_SPEED_CHANGE (6.3.3) carries two little-endian rates in bits a
 * second, upstream then downstream. Anything else is named by its code, and
 * a device that speaks RNDIS on the same endpoint is exactly that case.
 */
enum usb_notify_kind {
    USB_NOTIFY_MALFORMED,       /* short, not A1h, or data past the end */
    USB_NOTIFY_OTHER,           /* a notification this does not read */
    USB_NOTIFY_CONNECTION,      /* NETWORK_CONNECTION: the link */
    USB_NOTIFY_SPEED,           /* CONNECTION_SPEED_CHANGE: the two rates */
};

struct usb_notify {
    enum usb_notify_kind kind;
    uint8_t  code;              /* bNotificationCode, for OTHER */
    bool     up;                /* CONNECTION: wValue is 1 for connected */
    uint32_t upstream;          /* SPEED: bits a second, this end out */
    uint32_t downstream;        /* ...and in */
    unsigned length;            /* the whole of it, header and data */
};

/* Nothing outside `bytes[0 .. length)` is read. */
void usb_decode_notify(const uint8_t *bytes, unsigned length,
                       struct usb_notify *out);

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
