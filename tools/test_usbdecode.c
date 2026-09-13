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

    /* 4. A stick: not HID at all. */
    got = decode(stick, sizeof(stick));
    check(got.kind == USB_CONFIG_NOT_HID, "a stick is not 'not HID'");
    check(got.configuration == 1, "a stick: configuration value");

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

    if (fails == 0) {
        printf("PASS: %d checks on USB configuration descriptors (QEMU's "
               "mouse, HID 1.11's example, and the lengths a device can get "
               "wrong).\n", checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on USB configuration descriptors.\n",
           fails, checks + fails);
    return 1;
}
