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
#include <string.h>

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

/* HID 1.11 6.2.1: the HID descriptor, and in it each class descriptor's type
 * and length, three bytes to an entry - the Report descriptor's among them. */
#define DESC_HID            0x21u
#define DESC_REPORT         0x22u
#define HID_LENGTH          9u
#define HID_COUNT           5u          /* bNumDescriptors */
#define HID_ENTRIES         6u          /* bDescriptorType, wDescriptorLength */

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
    out->report_length = 0;

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
                out->report_length = 0;
            }
        } else if (d[1] == DESC_HID && d[0] >= HID_LENGTH && in_mouse) {
            unsigned k;

            /* Between the interface and its endpoints (HID 1.11 7.1), so it
             * is the mouse's own; a keyboard's before it was reset above. */
            for (k = 0; k < d[HID_COUNT] && HID_ENTRIES + 3u * (k + 1u) <= d[0];
                 k++) {
                const uint8_t *entry = d + HID_ENTRIES + 3u * k;

                if (entry[0] == DESC_REPORT) {
                    out->report_length =
                        (uint16_t)(entry[1] | (unsigned)entry[2] << 8);
                    break;
                }
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

/*
 * A mouse's Report descriptor, walked for its buttons and its movement.
 *
 * **Why it is read at all.** A boot mouse is asked for the boot protocol
 * (HID 1.11 7.2.6), whose report is fixed - a byte of buttons, a byte of X, a
 * byte of Y (B.2) - and QEMU's mouse honours the request. The ThinkPad's USB
 * mouse, a 04d9:fc38 "USB Gaming Mouse", took it without an error and went
 * on sending its own reports: moved right, the arrow went down, and up and
 * down did nothing. Byte 0 of its first report was 0, so no Report ID was in
 * front, and the likeliest layout is sixteen buttons in two bytes before X
 * and Y. Every other system reads the Report descriptor and does what it
 * says, which is what this is for.
 *
 * **Items** (6.2.2.2): a prefix of tag, type and size, then 0, 1, 2 or 4
 * bytes of data, low byte first. Long items (6.2.2.3) define nothing and are
 * stepped over. **Globals** (6.2.2.7) hold until changed - Usage Page, Report
 * Size, Report Count, Report ID - with Push and Pop keeping a stack of them.
 * **Locals** (6.2.2.8) - Usage, Usage Minimum and Maximum - belong to the next
 * Main item and end with it. A one- or two-byte usage is on the page in force
 * when the Main item is met; a four-byte one carries its own page in its high
 * half.
 *
 * **An Input item** (6.2.2.4) adds Report Size bits, Report Count times, to its
 * report. A constant one is padding and an array one names controls by index;
 * both are stepped over. A variable one takes its usages one to a field - the
 * last usage going on to any fields past them (6.2.2.8) - or Usage Minimum
 * upwards. X and Y are Generic Desktop's 0x30 and 0x31, and a button anything
 * on the Button page, 0x09: the numbers HID 1.11 E.10 encodes them with.
 *
 * **Signed or not** is the Logical Minimum's to say (5.8): negative, and the
 * field is two's complement; not, and it is unsigned. The Logical Minimum is
 * itself signed, in as many bytes as its item has.
 *
 * **Report IDs** (6.2.2.7): once one appears anywhere every report starts with
 * one, and the items after an ID belong to that report - so offsets are kept
 * for each ID, and the mouse is the first report with both X and Y. ID 0 is
 * reserved, and refused.
 *
 * **Refused rather than guessed**: an item whose data runs past the end, a Pop
 * with nothing pushed or a Push too deep, a report longer than 65535 bits,
 * and X or Y that is absolute or wider than 32 bits.
 */

/* 6.2.2.3: bTag 1111, bType 3, bSize 2. */
#define ITEM_LONG           0xFEu

/* 6.2.2.2: bType. */
#define ITEM_MAIN           0u
#define ITEM_GLOBAL         1u
#define ITEM_LOCAL          2u

/* 6.2.2.4: Input's tag, and its data bits 0 to 2. */
#define MAIN_INPUT          0x8u        /* 1000 00 nn */
#define INPUT_CONSTANT      0x1u
#define INPUT_VARIABLE      0x2u
#define INPUT_RELATIVE      0x4u

/* 6.2.2.7 */
#define GLOBAL_USAGE_PAGE   0x0u        /* 0000 01 nn */
#define GLOBAL_LOGICAL_MIN  0x1u        /* 0001 01 nn */
#define GLOBAL_REPORT_SIZE  0x7u        /* 0111 01 nn */
#define GLOBAL_REPORT_ID    0x8u        /* 1000 01 nn */
#define GLOBAL_REPORT_COUNT 0x9u        /* 1001 01 nn */
#define GLOBAL_PUSH         0xAu        /* 1010 01 nn */
#define GLOBAL_POP          0xBu        /* 1011 01 nn */

/* 6.2.2.8 */
#define LOCAL_USAGE         0x0u        /* 0000 10 nn */
#define LOCAL_USAGE_MIN     0x1u        /* 0001 10 nn */
#define LOCAL_USAGE_MAX     0x2u        /* 0010 10 nn */

/* E.10's values: page in the high half, usage in the low. */
#define PAGE_BUTTON         0x0009u
#define USAGE_X             0x00010030u
#define USAGE_Y             0x00010031u

#define STACK_DEPTH         4u
#define USAGES_KEPT         16u
#define BUTTONS_KEPT        16u
#define REPORT_BITS_MOST    0xFFFFu

struct item_globals {
    uint32_t page;
    uint32_t size;
    uint32_t count;
    uint32_t id;
    int32_t  logical_min;
};

/* One report's Input fields so far: its length, and what was found in it. */
struct report_seen {
    uint32_t bits;
    uint32_t buttons_at;
    uint32_t buttons;
    uint32_t x_at;
    uint32_t y_at;
    uint32_t x_bits;
    uint32_t y_bits;
    bool     has_x;
    bool     has_y;
    bool     relative_x;
    bool     relative_y;
    bool     x_signed;
    bool     y_signed;
};

/* A report ID is a byte. Static and cleared on every call, because 256 of
 * these would be most of a page of a server's stack. */
static struct report_seen seen[256];

/* An item's data as the signed number 5.8 makes it: two's complement in as
 * many bytes as the item has. */
static int32_t signed_of(uint32_t data, unsigned size)
{
    if (size == 1u && (data & 0x80u) != 0) {
        return (int32_t)data - 0x100;
    }

    if (size == 2u && (data & 0x8000u) != 0) {
        return (int32_t)data - 0x10000;
    }

    if (size == 4u && (data & 0x80000000u) != 0) {
        return -(int32_t)(~data) - 1;
    }

    return (int32_t)data;
}

/* A variable Input field's usage, taken as its Report Count went by. */
static void found_field(struct report_seen *s, uint32_t usage, uint32_t bit,
                        uint32_t size, bool relative, bool is_signed)
{
    if (usage == USAGE_X && !s->has_x && size <= 32u) {
        s->has_x = true;
        s->x_at = bit;
        s->x_bits = size;
        s->relative_x = relative;
        s->x_signed = is_signed;
    } else if (usage == USAGE_Y && !s->has_y && size <= 32u) {
        s->has_y = true;
        s->y_at = bit;
        s->y_bits = size;
        s->relative_y = relative;
        s->y_signed = is_signed;
    } else if ((usage >> 16) == PAGE_BUTTON && size == 1u) {
        if (s->buttons == 0) {
            s->buttons_at = bit;
        }

        /* In a row only: a button further on is some other control's. */
        if (s->buttons < BUTTONS_KEPT && bit == s->buttons_at + s->buttons) {
            s->buttons++;
        }
    }
}

void usb_decode_mouse_report(const uint8_t *bytes, unsigned length,
                             struct usb_mouse_report *out)
{
    struct item_globals g, stack[STACK_DEPTH];
    uint32_t usages[USAGES_KEPT];
    bool whole[USAGES_KEPT];
    uint32_t usage_min = 0, usage_max = 0;
    bool min_whole = false, have_min = false, have_max = false;
    bool uses_ids = false;
    unsigned depth = 0, nusages = 0, at = 0;
    int mouse = -1;
    const struct report_seen *r;

    memset(out, 0, sizeof(*out));
    memset(seen, 0, sizeof(seen));
    memset(&g, 0, sizeof(g));

    if (bytes == NULL) {
        return;
    }

    while (at < length) {
        uint8_t prefix = bytes[at];
        unsigned size, i;
        uint32_t data = 0;

        if (prefix == ITEM_LONG) {
            if (length - at < 3u || bytes[at + 1u] > length - at - 3u) {
                return;
            }

            at += 3u + bytes[at + 1u];
            continue;
        }

        size = (prefix & 0x3u) == 0x3u ? 4u : (prefix & 0x3u);

        if (size > length - at - 1u) {
            return;
        }

        for (i = 0; i < size; i++) {
            data |= (uint32_t)bytes[at + 1u + i] << (8u * i);
        }

        at += 1u + size;

        switch ((prefix >> 2) & 0x3u) {
        case ITEM_GLOBAL:
            switch (prefix >> 4) {
            case GLOBAL_USAGE_PAGE:
                g.page = data;
                break;
            case GLOBAL_LOGICAL_MIN:
                g.logical_min = signed_of(data, size);
                break;
            case GLOBAL_REPORT_SIZE:
                g.size = data;
                break;
            case GLOBAL_REPORT_COUNT:
                g.count = data;
                break;
            case GLOBAL_REPORT_ID:
                if (data == 0 || data > 255u) {
                    return;
                }

                g.id = data;
                uses_ids = true;
                break;
            case GLOBAL_PUSH:
                if (depth == STACK_DEPTH) {
                    return;
                }

                stack[depth++] = g;
                break;
            case GLOBAL_POP:
                if (depth == 0) {
                    return;
                }

                g = stack[--depth];
                break;
            default:
                break;
            }

            break;

        case ITEM_LOCAL:
            switch (prefix >> 4) {
            case LOCAL_USAGE:
                if (nusages < USAGES_KEPT) {
                    usages[nusages] = data;
                    whole[nusages] = size == 4u;
                    nusages++;
                }

                break;
            case LOCAL_USAGE_MIN:
                usage_min = data;
                min_whole = size == 4u;
                have_min = true;
                break;
            case LOCAL_USAGE_MAX:
                usage_max = data;
                have_max = true;
                break;
            default:
                break;
            }

            break;

        case ITEM_MAIN:
            if ((prefix >> 4) == MAIN_INPUT) {
                struct report_seen *s = &seen[g.id];
                uint64_t total = (uint64_t)g.size * g.count;

                if (s->bits + total > REPORT_BITS_MOST) {
                    return;
                }

                if ((data & INPUT_CONSTANT) == 0
                    && (data & INPUT_VARIABLE) != 0 && g.size != 0) {
                    /* At most 65535 fields: size is at least 1 and the total
                     * was bounded above. */
                    for (i = 0; i < g.count; i++) {
                        uint32_t usage;
                        bool own_page;

                        if (nusages > 0) {
                            unsigned k = i < nusages ? i : nusages - 1u;

                            usage = usages[k];
                            own_page = whole[k];
                        } else if (have_min && have_max
                                   && usage_min + i <= usage_max) {
                            usage = usage_min + i;
                            own_page = min_whole;
                        } else {
                            break;
                        }

                        if (!own_page) {
                            usage = (g.page << 16) | (usage & 0xFFFFu);
                        }

                        found_field(s, usage, s->bits + i * g.size, g.size,
                                    (data & INPUT_RELATIVE) != 0,
                                    g.logical_min < 0);
                    }
                }

                s->bits += (uint32_t)total;

                if (mouse < 0 && s->has_x && s->has_y) {
                    mouse = (int)g.id;
                }
            }

            /* Local items end at every Main item (6.2.2.8). */
            nusages = 0;
            have_min = false;
            have_max = false;
            break;

        default:
            break;
        }
    }

    /* Fields before the first ID, in a descriptor that uses IDs, belong to
     * no report that can arrive. */
    if (mouse < 0 || (uses_ids && mouse == 0)) {
        return;
    }

    r = &seen[mouse];

    if (!r->relative_x || !r->relative_y) {
        return;
    }

    out->ok = true;
    out->id = (uint8_t)(uses_ids ? mouse : 0);
    out->buttons = (uint8_t)r->buttons;
    out->buttons_at = (uint16_t)r->buttons_at;
    out->x_at = (uint16_t)r->x_at;
    out->y_at = (uint16_t)r->y_at;
    out->x_bits = (uint8_t)r->x_bits;
    out->y_bits = (uint8_t)r->y_bits;
    out->x_signed = r->x_signed;
    out->y_signed = r->y_signed;
    out->bits = (uint16_t)r->bits;
}

int32_t usb_report_field(const uint8_t *report, unsigned length, unsigned at,
                         unsigned bits, bool is_signed)
{
    uint32_t value = 0;
    unsigned i;

    if (report == NULL || bits == 0 || bits > 32u || length > 0x10000000u
        || at > length * 8u || bits > length * 8u - at) {
        return 0;
    }

    for (i = 0; i < bits; i++) {
        unsigned bit = at + i;

        value |= (uint32_t)((report[bit / 8u] >> (bit % 8u)) & 1u) << i;
    }

    if (is_signed && bits < 32u && ((value >> (bits - 1u)) & 1u) != 0) {
        value |= ~0u << bits;
    }

    /* Two's complement without leaning on the conversion to a signed type. */
    return (value & 0x80000000u) != 0 ? -(int32_t)(~value) - 1
                                      : (int32_t)value;
}
