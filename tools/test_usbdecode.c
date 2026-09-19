/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * USB configuration descriptors, asked the awkward questions on the host.
 *
 * The xHCI driver reads a device's configuration to find a mouse in it, and
 * QEMU's mouse sends exactly one configuration, well formed. So what QEMU
 * cannot produce is built here a byte at a time: a length of zero, a
 * descriptor that runs past the end, a total longer than what arrived, a
 * mouse whose endpoint faces the wrong way - and two well-formed ones, each
 * with a source that is not this project. QEMU's is laid out from the
 * declarations in `hw/usb/dev-hid.c`, with the HID descriptor between the
 * interface and its endpoint as HID 1.11 F.3 requires; the keyboard-and-mouse
 * pair is HID 1.11's Appendix E, byte for byte.
 *
 * Same split as `tools/test_smbiosdecode.c`, for the same reason.
 */

#include <stdio.h>
#include <string.h>

#include "../user/servers/usb_decode.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

/* QEMU 11.1.1's usb-mouse at high speed: configuration, interface, HID
 * descriptor, endpoint. 34 bytes. */
static const uint8_t qemu_mouse[] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x06, 0xa0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
    0x09, 0x21, 0x01, 0x00, 0x00, 0x01, 0x22, 0x34, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x04, 0x00, 0x07,
};

/* Where the pieces of `qemu_mouse` begin. */
#define AT_IFACE    9
#define AT_HID      18
#define AT_EP       27

/* HID 1.11 Appendix E: E.2 to E.5, a boot keyboard, then E.7 to E.9, a boot
 * mouse. 59 bytes. */
static const uint8_t hid_example[] = {
    0x09, 0x02, 0x3b, 0x00, 0x02, 0x01, 0x00, 0xa0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x01, 0x01, 0x00, 0x01, 0x22, 0x3f, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0a,
    0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
    0x09, 0x21, 0x01, 0x01, 0x00, 0x01, 0x22, 0x32, 0x00,
    0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x0a,
};

/* A stick: mass storage, bulk only, two bulk endpoints. 32 bytes. */
static const uint8_t stick[] = {
    0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,
    0x07, 0x05, 0x81, 0x02, 0x00, 0x02, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x00, 0x02, 0x00,
};

/* Where the pieces of `stick` begin. */
#define AT_STICK_IFACE  9
#define AT_STICK_IN     18
#define AT_STICK_OUT    25

/* QEMU 11.1.1's usb-storage at SuperSpeed, laid out from the declarations in
 * `hw/usb/dev-storage.c`: 1024-byte bulk endpoints, IN 1 and OUT 2, each
 * followed by a SuperSpeed Endpoint Companion with a burst of 15. 44 bytes. */
static const uint8_t superspeed_stick[] = {
    0x09, 0x02, 0x2c, 0x00, 0x01, 0x01, 0x06, 0xc0, 0x00,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,
    0x07, 0x05, 0x81, 0x02, 0x00, 0x04, 0x00,
    0x06, 0x30, 0x0f, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x00, 0x04, 0x00,
    0x06, 0x30, 0x0f, 0x00, 0x00, 0x00,
};

/* Where each companion's bMaxBurst is in `superspeed_stick`. */
#define AT_IN_BURST     27
#define AT_OUT_BURST    40

/*
 * An Xbox 360 controller, 045e:028e. Interface 0 byte for byte as its
 * published dump gives it - FFh/5Dh/01h, its 17-byte descriptor of type 21h,
 * interrupt IN 81h of 32 bytes every 4 frames and interrupt OUT 01h - and
 * interface 1, the headset's (FFh/5Dh/03h), shortened to one endpoint of its
 * four, its 27-byte descriptor's first fourteen bytes as the dump shows them
 * and the rest zeroes. The real pad has two interfaces more; they are not
 * the pad and the walk never reaches them. 92 bytes.
 */
static const uint8_t xbox360[] = {
    0x09, 0x02, 0x5c, 0x00, 0x02, 0x01, 0x00, 0xa0, 0xfa,
    0x09, 0x04, 0x00, 0x00, 0x02, 0xff, 0x5d, 0x01, 0x00,
    0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0x00, 0x00, 0x00, 0x00,
    0x13, 0x01, 0x08, 0x00, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,
    0x07, 0x05, 0x01, 0x03, 0x20, 0x00, 0x08,
    0x09, 0x04, 0x01, 0x00, 0x04, 0xff, 0x5d, 0x03, 0x00,
    0x1b, 0x21, 0x00, 0x01, 0x01, 0x01, 0x82, 0x40, 0x01, 0x02, 0x20, 0x16,
    0x83, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
    0x07, 0x05, 0x82, 0x03, 0x20, 0x00, 0x02,
};

_Static_assert(sizeof(xbox360) == 0x5c, "the 360's wTotalLength is its size");

/*
 * An Xbox One or Series pad, laid out from Microsoft's [MS-GIPUSB] rather
 * than a dump: interface 0, FFh/47h/D0h, with two 64-byte interrupt
 * endpoints every 4 ms - the OUT listed first - then the audio interface,
 * the same class, whose alternate 1 has isochronous endpoints, and a third
 * with none. 73 bytes.
 */
static const uint8_t xboxone[] = {
    0x09, 0x02, 0x49, 0x00, 0x03, 0x01, 0x00, 0xa0, 0xfa,
    0x09, 0x04, 0x00, 0x00, 0x02, 0xff, 0x47, 0xd0, 0x00,
    0x07, 0x05, 0x02, 0x03, 0x40, 0x00, 0x04,
    0x07, 0x05, 0x82, 0x03, 0x40, 0x00, 0x04,
    0x09, 0x04, 0x01, 0x00, 0x00, 0xff, 0x47, 0xd0, 0x00,
    0x09, 0x04, 0x01, 0x01, 0x02, 0xff, 0x47, 0xd0, 0x00,
    0x07, 0x05, 0x03, 0x01, 0xe4, 0x00, 0x01,
    0x07, 0x05, 0x83, 0x01, 0x40, 0x00, 0x01,
    0x09, 0x04, 0x02, 0x00, 0x00, 0xff, 0x47, 0xd0, 0x00,
};

_Static_assert(sizeof(xboxone) == 0x49, "the One's wTotalLength is its size");

#define AT_ONE_OUT      20              /* interface 0's OUT's address */

#define AT_PAD_PROTOCOL 16              /* interface 0's bInterfaceProtocol */
#define AT_PAD_IN       37              /* its IN endpoint's address */

static struct usb_config decode(const uint8_t *bytes, unsigned length)
{
    struct usb_config got;

    usb_decode_config(bytes, length, &got);
    return got;
}

/* `qemu_mouse` with one byte changed. */
static struct usb_config mouse_with(unsigned at, uint8_t value)
{
    uint8_t copy[sizeof(qemu_mouse)];

    memcpy(copy, qemu_mouse, sizeof(copy));
    copy[at] = value;
    return decode(copy, sizeof(copy));
}

/* `stick` with one byte changed. */
static struct usb_config stick_with(unsigned at, uint8_t value)
{
    uint8_t copy[sizeof(stick)];

    memcpy(copy, stick, sizeof(copy));
    copy[at] = value;
    return decode(copy, sizeof(copy));
}

static int is_stick(struct usb_config got, unsigned in, unsigned out,
                    unsigned in_packet, unsigned out_packet, unsigned in_burst,
                    unsigned out_burst)
{
    return got.kind == USB_CONFIG_BULK_ONLY && got.bulk_in == in
        && got.bulk_out == out && got.bulk_in_packet == in_packet
        && got.bulk_out_packet == out_packet && got.bulk_in_burst == in_burst
        && got.bulk_out_burst == out_burst;
}

static int no_stick(struct usb_config got)
{
    return got.bulk_in == 0 && got.bulk_out == 0 && got.bulk_in_packet == 0
        && got.bulk_out_packet == 0 && got.storage_interface == 0;
}

/* HID 1.11 E.10, byte for byte: three buttons, five bits of padding, X and
 * Y a byte each. 50 bytes, the length E.8's HID descriptor gives it. */
static const uint8_t e10_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05,
    0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81,
    0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0,
};

/* Where E.10's pieces are: the Usage Page before X, the X and Y Report
 * Size's data, and the X and Y Input's data. */
#define E10_PAGE_XY     32
#define E10_SIZE_XY     43
#define E10_INPUT_XY    47
#define E10_LOGICAL_MIN 39

/* QEMU 11.1.1's usb-mouse, from `qemu_mouse_hid_report_descriptor` in
 * `hw/usb/dev-hid.c`: five buttons, three bits of padding, X, Y and a wheel.
 * 52 bytes, the length `qemu_mouse`'s HID descriptor gives it. */
static const uint8_t qemu_report[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03,
    0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xc0, 0xc0,
};

/*
 * **Made here, not read off a mouse**: sixteen buttons in two bytes, then X
 * and Y in sixteen bits each, then a wheel - a gaming mouse's shape, and the
 * likeliest reading of the ThinkPad's 04d9:fc38, whose reports moved the
 * arrow down when it moved right. Read as a boot report, the second byte is
 * the upper buttons, always 0, and the third is X.
 */
static const uint8_t sixteen_buttons[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x10, 0x75, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f,
    0x09, 0x30, 0x09, 0x31, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xc0, 0xc0,
};

/* E.10 as Report ID 1, then a consumer control as Report ID 2: a
 * sixteen-bit array, which is stepped over. */
static const uint8_t with_id[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0,
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x02, 0x15, 0x00, 0x26, 0xff,
    0x03, 0x19, 0x00, 0x2a, 0xff, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xc0,
};

/*
 * A keyboard as Report ID 1 - eight modifier bits on the keyboard page, six
 * key bytes as an array - and the mouse as Report ID 2: five buttons and
 * three bits of padding, X and Y as four-byte usages with the Button page
 * still in force, and a Push, a four-bit field and a Pop, after which a
 * constant field is sixteen bits again. 44 bits of report.
 */
static const uint8_t keyboard_then_mouse[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x08, 0x81, 0x02,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00, 0xc0,
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05,
    0x75, 0x01, 0x81, 0x02,
    0x95, 0x03, 0x81, 0x01,
    0x0b, 0x30, 0x00, 0x01, 0x00, 0x0b, 0x31, 0x00, 0x01, 0x00,
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xa4, 0x75, 0x04, 0x95, 0x01, 0x09, 0x38, 0x81, 0x06, 0xb4,
    0x81, 0x01,
    0xc0,
};

static struct usb_mouse_report layout(const uint8_t *bytes, unsigned length)
{
    struct usb_mouse_report got;

    usb_decode_mouse_report(bytes, length, &got);
    return got;
}

static int laid_out(struct usb_mouse_report r, unsigned id, unsigned buttons,
                    unsigned buttons_at, unsigned x_at, unsigned x_bits,
                    unsigned y_at, unsigned y_bits, unsigned bits)
{
    return r.ok && r.id == id && r.buttons == buttons
        && r.buttons_at == buttons_at && r.x_at == x_at && r.x_bits == x_bits
        && r.y_at == y_at && r.y_bits == y_bits && r.bits == bits;
}

/* E.10 with `prefix` in front of it, into `out`; the length. */
static unsigned before_e10(uint8_t *out, const uint8_t *prefix, unsigned n)
{
    memcpy(out, prefix, n);
    memcpy(out + n, e10_mouse, sizeof(e10_mouse));
    return n + sizeof(e10_mouse);
}

int main(void)
{
    struct usb_config got;
    uint8_t longer[sizeof(qemu_mouse) + 1];

    /* 1. QEMU's mouse: everything the driver needs to read it. */
    got = decode(qemu_mouse, sizeof(qemu_mouse));
    check(got.kind == USB_CONFIG_BOOT_MOUSE,
          "QEMU's mouse is not a boot mouse");
    check(got.configuration == 1, "QEMU's mouse: configuration value");
    check(got.interface == 0, "QEMU's mouse: interface number");
    check(got.endpoint == 1, "QEMU's mouse: endpoint number");
    check(got.packet == 4, "QEMU's mouse: packet size");
    check(got.extra == 0, "QEMU's mouse: additional transactions");
    check(got.interval == 7, "QEMU's mouse: interval");

    /* 2. A keyboard first and the mouse second: the mouse's interface and
     *    endpoint, not the keyboard's, and the keyboard as the first HID. */
    got = decode(hid_example, sizeof(hid_example));
    check(got.kind == USB_CONFIG_BOOT_MOUSE,
          "HID 1.11's example: the mouse behind the keyboard was not found");
    check(got.interface == 1 && got.endpoint == 2,
          "HID 1.11's example: took the keyboard's interface or endpoint");
    check(got.packet == 8 && got.interval == 10,
          "HID 1.11's example: packet size or interval");
    check(got.hid_subclass == 1 && got.hid_protocol == 1,
          "HID 1.11's example: the first HID interface is its keyboard");

    /* 3. The keyboard alone: HID, and no mouse in it. */
    {
        uint8_t keyboard[34];

        memcpy(keyboard, hid_example, sizeof(keyboard));
        keyboard[2] = sizeof(keyboard);
        keyboard[4] = 1;
        got = decode(keyboard, sizeof(keyboard));
        check(got.kind == USB_CONFIG_HID_OTHER,
              "a keyboard alone is not HID without a mouse");
        check(got.hid_subclass == 1 && got.hid_protocol == 1,
              "a keyboard alone: its subclass and protocol");
    }

    /* 3b. An Xbox 360 controller: its gamepad interface and its IN, and the
     *     descriptor of type 21h between them not taken for HID's. */
    {
        uint8_t copy[sizeof(xbox360)];

        got = decode(xbox360, sizeof(xbox360));
        check(got.kind == USB_CONFIG_XBOX360 && got.interface == 0
              && got.endpoint == 1 && got.packet == 32 && got.interval == 4
              && got.report_length == 0,
              "an Xbox 360 controller was not its interface 0 with IN 1, 32 "
              "bytes every 4");

        memcpy(copy, xbox360, sizeof(copy));
        copy[AT_PAD_PROTOCOL] = 0x03;       /* a headset's, not a pad's */
        got = decode(copy, sizeof(copy));
        check(got.kind == USB_CONFIG_NEITHER,
              "an Xbox 360 interface of protocol 03h, a headset's, was "
              "taken for the pad");

        memcpy(copy, xbox360, sizeof(copy));
        copy[AT_PAD_IN] = 0x01;             /* the IN endpoint made OUT */
        got = decode(copy, sizeof(copy));
        check(got.kind != USB_CONFIG_XBOX360,
              "a pad with only OUT endpoints was taken for one to read");
    }

    /* 3c. An Xbox One pad: both interrupt endpoints, whichever comes first,
     *     and not the audio interface's isochronous ones. */
    {
        uint8_t copy[sizeof(xboxone)];

        got = decode(xboxone, sizeof(xboxone));
        check(got.kind == USB_CONFIG_XBOXONE && got.interface == 0
              && got.endpoint == 2 && got.packet == 64 && got.interval == 4
              && got.out_endpoint == 2 && got.out_packet == 64
              && got.out_interval == 4,
              "an Xbox One pad was not interface 0 with IN 2 and OUT 2, 64 "
              "bytes every 4");

        memcpy(copy, xboxone, sizeof(copy));
        copy[AT_ONE_OUT] = 0x81;            /* its OUT made a second IN */
        got = decode(copy, sizeof(copy));
        check(got.kind != USB_CONFIG_XBOXONE,
              "an Xbox One pad with no interrupt OUT was taken for one - the "
              "host could never tell it to start");
    }

    /* 4. A stick: SCSI over Bulk-Only, bulk IN 1 and OUT 2, 512 bytes. */
    got = decode(stick, sizeof(stick));
    check(is_stick(got, 1, 2, 512, 512, 0, 0),
          "a high-speed stick is not Bulk-Only with IN 1 and OUT 2 of 512");
    check(got.configuration == 1 && got.storage_interface == 0,
          "a stick: configuration value or interface number");
    check(got.storage_subclass == 0x06 && got.storage_protocol == 0x50,
          "a stick: its subclass and protocol");

    /* ...and QEMU's at SuperSpeed, each companion's burst its endpoint's. */
    check(is_stick(decode(superspeed_stick, sizeof(superspeed_stick)),
                   1, 2, 1024, 1024, 15, 15),
          "QEMU's SuperSpeed stick is not 1024-byte endpoints with bursts "
          "of 15");

    {
        uint8_t bursts[sizeof(superspeed_stick)];

        memcpy(bursts, superspeed_stick, sizeof(bursts));
        bursts[AT_IN_BURST] = 3;
        bursts[AT_OUT_BURST] = 7;
        check(is_stick(decode(bursts, sizeof(bursts)), 1, 2, 1024, 1024, 3, 7),
              "a companion's burst was given to the other endpoint");

        bursts[AT_OUT_BURST] = 16;
        got = decode(bursts, sizeof(bursts));
        check(got.kind == USB_CONFIG_STORAGE_OTHER && no_stick(got),
              "a burst of 16 was kept for a controller");
    }

    /* OUT before IN: each is still its own direction's. */
    {
        uint8_t swapped[sizeof(stick)];

        memcpy(swapped, stick, AT_STICK_IN);
        memcpy(swapped + AT_STICK_IN, stick + AT_STICK_OUT, 7);
        memcpy(swapped + AT_STICK_OUT, stick + AT_STICK_IN, 7);
        check(is_stick(decode(swapped, sizeof(swapped)), 1, 2, 512, 512, 0, 0),
              "a stick with its OUT endpoint first had its directions mixed up");
    }

    /* Mass storage this cannot speak to: said to be that, with no endpoints. */
    got = stick_with(AT_STICK_IFACE + 7, 0x62);
    check(got.kind == USB_CONFIG_STORAGE_OTHER && got.storage_protocol == 0x62
          && no_stick(got),
          "USB Attached SCSI (62h) was taken for Bulk-Only");
    check(stick_with(AT_STICK_IFACE + 6, 0x00).kind == USB_CONFIG_STORAGE_OTHER,
          "a subclass of 00h was taken for SCSI transparent");
    check(stick_with(AT_STICK_IFACE + 3, 1).kind == USB_CONFIG_STORAGE_OTHER,
          "a stick at alternate setting 1 was taken");
    check(stick_with(AT_STICK_IN + 3, 0x03).kind == USB_CONFIG_STORAGE_OTHER,
          "an interrupt endpoint was taken for bulk IN");
    check(stick_with(AT_STICK_OUT + 2, 0x80).kind == USB_CONFIG_STORAGE_OTHER,
          "endpoint 0 was taken for bulk OUT");
    check(stick_with(AT_STICK_OUT + 2, 0x82).kind == USB_CONFIG_STORAGE_OTHER,
          "two IN endpoints were taken for an IN and an OUT");
    check(stick_with(AT_STICK_IN + 5, 0x00).kind == USB_CONFIG_STORAGE_OTHER,
          "a bulk endpoint with a packet size of 0 was taken");

    /* A keyboard first and a stick behind it: the stick is what is used. */
    {
        uint8_t both[34 + sizeof(stick) - 9];

        memcpy(both, hid_example, 34);
        memcpy(both + 34, stick + 9, sizeof(stick) - 9);
        both[2] = sizeof(both);
        both[4] = 2;
        both[34 + 2] = 1;                   /* the stick is interface 1 */
        got = decode(both, sizeof(both));
        check(is_stick(got, 1, 2, 512, 512, 0, 0)
              && got.storage_interface == 1,
              "a keyboard and a stick: the stick behind it was not found");
    }

    /* QEMU's mouse with a stick behind it: the mouse is taken, and the
     * stick's fields are left empty rather than half-filled. */
    {
        uint8_t mouse_first[sizeof(qemu_mouse) + sizeof(stick) - 9];

        memcpy(mouse_first, qemu_mouse, sizeof(qemu_mouse));
        memcpy(mouse_first + sizeof(qemu_mouse), stick + 9, sizeof(stick) - 9);
        mouse_first[2] = sizeof(mouse_first);
        mouse_first[4] = 2;
        mouse_first[sizeof(qemu_mouse) + 2] = 1;
        got = decode(mouse_first, sizeof(mouse_first));
        check(got.kind == USB_CONFIG_BOOT_MOUSE && no_stick(got),
              "a mouse with a stick behind it was not taken as the mouse");
    }

    /* Neither HID nor mass storage: a hub's interface, class 09h. */
    {
        uint8_t hub[sizeof(stick)];

        memcpy(hub, stick, sizeof(hub));
        hub[AT_STICK_IFACE + 5] = 0x09;
        hub[AT_STICK_IFACE + 6] = 0x00;
        hub[AT_STICK_IFACE + 7] = 0x00;
        check(decode(hub, sizeof(hub)).kind == USB_CONFIG_NEITHER,
              "a hub's interface was taken for HID or mass storage");
    }

    /* 5. Lengths a device can get wrong, each of which must end the walk. */
    check(mouse_with(AT_IFACE, 0).kind == USB_CONFIG_MALFORMED,
          "a descriptor of length 0 was walked");
    check(mouse_with(AT_IFACE, 1).kind == USB_CONFIG_MALFORMED,
          "a descriptor of length 1 was walked");
    check(mouse_with(AT_EP, 0x20).kind == USB_CONFIG_MALFORMED,
          "a descriptor running past the end was walked");
    check(decode(qemu_mouse, sizeof(qemu_mouse) - 1).kind
          == USB_CONFIG_MALFORMED,
          "a total longer than what arrived was believed");
    check(mouse_with(2, 30).kind == USB_CONFIG_MALFORMED,
          "a total that cuts the endpoint in half was walked");
    check(mouse_with(2, 8).kind == USB_CONFIG_MALFORMED,
          "a total shorter than the configuration itself was believed");
    check(decode(qemu_mouse, 8).kind == USB_CONFIG_MALFORMED,
          "eight bytes were read as a configuration");
    check(decode(NULL, 34).kind == USB_CONFIG_MALFORMED,
          "no bytes at all were read as a configuration");
    check(mouse_with(1, 0x01).kind == USB_CONFIG_MALFORMED,
          "a device descriptor was read as a configuration");

    /* More arrived than the total: the total is what is read. */
    memcpy(longer, qemu_mouse, sizeof(qemu_mouse));
    longer[sizeof(qemu_mouse)] = 0;
    check(decode(longer, sizeof(longer)).kind == USB_CONFIG_BOOT_MOUSE,
          "a byte after the total was walked as a descriptor");

    /* 6. A mouse interface this cannot read. */
    check(mouse_with(AT_EP + 2, 0x01).kind == USB_CONFIG_HID_OTHER,
          "an OUT endpoint was taken for the mouse's reports");
    check(mouse_with(AT_EP + 3, 0x02).kind == USB_CONFIG_HID_OTHER,
          "a bulk endpoint was taken for the mouse's reports");
    check(mouse_with(AT_EP + 2, 0x80).kind == USB_CONFIG_HID_OTHER,
          "endpoint 0 was taken for the mouse's reports");
    check(mouse_with(AT_EP + 4, 0x00).kind == USB_CONFIG_HID_OTHER,
          "an endpoint with no packet size was taken");
    check(mouse_with(AT_IFACE + 3, 1).kind == USB_CONFIG_HID_OTHER,
          "a mouse at alternate setting 1 was taken");
    check(mouse_with(AT_IFACE + 7, 0).kind == USB_CONFIG_HID_OTHER,
          "a HID interface with no protocol was taken for a mouse");

    /* 7. High speed's extra transactions: the size is bits 10:0 alone. */
    got = mouse_with(AT_EP + 5, 0x08);
    check(got.kind == USB_CONFIG_BOOT_MOUSE && got.packet == 4
          && got.extra == 1,
          "wMaxPacketSize 0x0804 is not 4 bytes with one extra transaction");

    /* 8. How long the Report descriptor is, from the mouse's own HID
     *    descriptor - in HID 1.11's pair the mouse's 0x32, not the 0x3f of
     *    the keyboard in front of it. */
    got = decode(qemu_mouse, sizeof(qemu_mouse));
    check(got.report_length == 52, "QEMU's mouse: its Report descriptor's length");
    got = decode(hid_example, sizeof(hid_example));
    check(got.report_length == 0x32,
          "HID 1.11's example: the keyboard's Report descriptor length was "
          "taken for the mouse's");

    /* 9. Report descriptors: where the buttons, X and Y are. */
    {
        static const uint8_t long_item[] = { 0xfe, 0x02, 0xf0, 0x11, 0x22 };
        static const uint8_t id_zero[] = { 0x85, 0x00 };
        static const uint8_t pop[] = { 0xb4 };
        static const uint8_t five_pushes[] = { 0xa4, 0xa4, 0xa4, 0xa4, 0xa4 };
        static const uint8_t moved[] = { 0x01, 0x00, 0x03, 0x00, 0xfe, 0xff,
                                         0x00 };
        uint8_t buf[128];
        struct usb_mouse_report r;
        unsigned n;

        check(laid_out(layout(e10_mouse, sizeof(e10_mouse)),
                       0, 3, 0, 8, 8, 16, 8, 24),
              "HID 1.11 E.10: not three buttons, then X and Y a byte each");
        check(laid_out(layout(qemu_report, sizeof(qemu_report)),
                       0, 5, 0, 8, 8, 16, 8, 32),
              "QEMU's mouse: not five buttons, padding, X, Y and a wheel");

        r = layout(sixteen_buttons, sizeof(sixteen_buttons));
        check(laid_out(r, 0, 16, 0, 16, 16, 32, 16, 56),
              "sixteen buttons: X and Y not after two bytes of buttons, 16 "
              "bits each");

        /* Left held, 3 to the right, 2 up, the wheel still. */
        check(usb_report_field(moved, sizeof(moved), r.buttons_at, 3, false)
              == 1
              && usb_report_field(moved, sizeof(moved), r.x_at, r.x_bits,
                                  r.x_signed) == 3
              && usb_report_field(moved, sizeof(moved), r.y_at, r.y_bits,
                                  r.y_signed) == -2,
              "sixteen buttons: a report read by its layout is not the left "
              "button, 3 right and 2 up");

        /* 5.8: signed because the Logical Minimum is negative - E.10's -127
         * in one byte - and unsigned when that minimum is made 0. */
        r = layout(e10_mouse, sizeof(e10_mouse));
        check(r.x_signed && r.y_signed,
              "HID 1.11 E.10: X and Y from a Logical Minimum of -127 are not "
              "signed");
        memcpy(buf, e10_mouse, sizeof(e10_mouse));
        buf[E10_LOGICAL_MIN] = 0x00;
        r = layout(buf, sizeof(e10_mouse));
        check(r.ok && !r.x_signed && !r.y_signed,
              "X and Y from a Logical Minimum of 0 were taken as signed");

        check(laid_out(layout(with_id, sizeof(with_id)),
                       1, 3, 0, 8, 8, 16, 8, 24),
              "a mouse as Report ID 1 beside a consumer control as ID 2");
        check(laid_out(layout(keyboard_then_mouse, sizeof(keyboard_then_mouse)),
                       2, 5, 0, 8, 8, 16, 8, 44),
              "a keyboard as Report ID 1 and the mouse as ID 2, with four-byte "
              "usages and a Push and a Pop");

        /* Absolute X and Y: a tablet, which the pointer takes from no driver. */
        memcpy(buf, e10_mouse, sizeof(e10_mouse));
        buf[E10_INPUT_XY] = 0x02;
        check(!layout(buf, sizeof(e10_mouse)).ok,
              "absolute X and Y were taken for a mouse");

        /* X and Y 33 bits wide. */
        memcpy(buf, e10_mouse, sizeof(e10_mouse));
        buf[E10_SIZE_XY] = 33;
        check(!layout(buf, sizeof(e10_mouse)).ok,
              "X and Y wider than 32 bits were taken");

        /* The Usage Page after the usages and before the Input: the page in
         * force at the Main item is theirs (6.2.2.8). */
        memcpy(buf, e10_mouse, sizeof(e10_mouse));
        memcpy(buf + E10_PAGE_XY, e10_mouse + E10_PAGE_XY + 2, 4);
        memcpy(buf + E10_PAGE_XY + 4, e10_mouse + E10_PAGE_XY, 2);
        check(laid_out(layout(buf, sizeof(e10_mouse)), 0, 3, 0, 8, 8, 16, 8,
                       24),
              "a Usage Page given after X and Y's usages was not theirs");

        /* E.10 cut after its last Input's prefix. The byte after the cut is
         * still in the array, so a walk that read it would find X and Y. */
        check(!layout(e10_mouse, E10_INPUT_XY).ok,
              "an item whose data runs past the descriptor's end was read");

        n = before_e10(buf, long_item, sizeof(long_item));
        check(laid_out(layout(buf, n), 0, 3, 0, 8, 8, 16, 8, 24),
              "a long item in front of E.10 was not stepped over");

        n = before_e10(buf, id_zero, sizeof(id_zero));
        check(!layout(buf, n).ok, "Report ID 0, which is reserved, was taken");

        n = before_e10(buf, pop, sizeof(pop));
        check(!layout(buf, n).ok, "a Pop with nothing pushed was taken");

        n = before_e10(buf, five_pushes, sizeof(five_pushes));
        check(!layout(buf, n).ok, "a Push five deep was taken");

        check(!layout(NULL, 10).ok && !layout(e10_mouse, 0).ok,
              "no descriptor at all was read as a mouse");
    }

    /* 10. Fields: across bytes, signed and not, and past the end. */
    {
        static const uint8_t bits[] = { 0xf0, 0xff, 0x78, 0x56, 0x34, 0x12 };

        check(usb_report_field(bits, sizeof(bits), 4, 12, false) == 0xfff
              && usb_report_field(bits, sizeof(bits), 4, 12, true) == -1,
              "a twelve-bit field across two bytes");
        check(usb_report_field(bits, sizeof(bits), 16, 32, false)
              == 0x12345678,
              "a 32-bit field, least significant byte first");
        check(usb_report_field(bits, sizeof(bits), 3, 1, false) == 0
              && usb_report_field(bits, sizeof(bits), 4, 1, false) == 1,
              "bit 3 of 0xf0 is not 0, or bit 4 not 1");
        check(usb_report_field(bits, sizeof(bits), 44, 8, false) == 0,
              "a field running past the report's end was read");
        check(usb_report_field(bits, sizeof(bits), 0, 0, false) == 0
              && usb_report_field(bits, sizeof(bits), 0, 33, false) == 0,
              "a field of 0 or of 33 bits was read");
    }

    if (fails == 0) {
        printf("PASS: %d checks on USB configuration and report descriptors "
               "(QEMU's mouse, HID 1.11's examples, a sixteen-button mouse, "
               "Report IDs, sticks at high speed and SuperSpeed, and the "
               "lengths a device can get wrong).\n",
               checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on USB configuration and report "
           "descriptors.\n", fails, checks + fails);
    return 1;
}
