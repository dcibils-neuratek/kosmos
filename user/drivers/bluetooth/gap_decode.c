/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What is nearby, decoded. `gap_decode.h` says what and why.
 */

#include "gap_decode.h"

#include <string.h>

#include "../../include/bytes.h"

/* Data types (CSS Part A 1). */
#define AD_FLAGS              0x01u
#define AD_UUID16_SOME        0x02u
#define AD_UUID16_ALL         0x03u
#define AD_NAME_SHORT         0x08u
#define AD_NAME_COMPLETE      0x09u
#define AD_APPEARANCE         0x19u

#define UUID_HID              0x1812u

/* An advertisement that may be connected to (7.7.65.2's Event_Type): one
 * connectable and undirected, or directed at this host. */
#define ADV_IND               0x00u
#define ADV_DIRECT_IND        0x01u

void gap_copy_name(char *out, unsigned room, const uint8_t *name,
                   unsigned length)
{
    unsigned n = 0, i;

    if (room == 0) {
        return;
    }

    for (i = 0; i < length && name[i] != 0; i++) {
        n++;
    }

    if (n > room - 1u) {
        n = room - 1u;

        /* Back to where a character starts: a continuation byte is
         * 10xxxxxx, and the cut goes before the byte that is not. */
        while (n > 0 && (name[n] & 0xC0u) == 0x80u) {
            n--;
        }
    }

    memcpy(out, name, n);
    out[n] = '\0';
}

void gap_read_data(const uint8_t *data, unsigned length, struct gap_found *f)
{
    unsigned at = 0;

    while (at < length) {
        unsigned len = data[at];
        const uint8_t *v;
        unsigned n, i;

        /* A length of zero ends the significant part (Volume 3 Part C
         * 8); one past the end ends the reading. */
        if (len == 0 || at + 1u + len > length) {
            break;
        }

        v = data + at + 2;
        n = len - 1u;

        switch (data[at + 1]) {
        case AD_NAME_COMPLETE:
            gap_copy_name(f->name, sizeof f->name, v, n);
            f->name_complete = true;
            break;

        case AD_NAME_SHORT:
            if (!f->name_complete) {
                gap_copy_name(f->name, sizeof f->name, v, n);
            }
            break;

        case AD_APPEARANCE:
            if (n >= 2u) {
                f->appearance = get_le16(v);
                f->has_appearance = true;
            }
            break;

        case AD_UUID16_SOME:
        case AD_UUID16_ALL:
            for (i = 0; i + 2u <= n; i += 2u) {
                if (get_le16(v + i) == UUID_HID) {
                    f->hid = true;
                }
            }
            break;

        default:
            break;
        }

        at += 1u + len;
    }
}

/* One Classic device, its fields from `p`; `rssi` and the EIR when the
 * event carries them. */
static void classic(const uint8_t *p, unsigned cod_at, int rssi_at,
                    const uint8_t *eir, unsigned eir_length,
                    gap_each each, void *ctx)
{
    struct gap_found f;

    memset(&f, 0, sizeof f);
    memcpy(f.address, p, 6);
    f.transport = GAP_CLASSIC;
    f.connectable = true;
    f.cod = (uint32_t)p[cod_at] | (uint32_t)p[cod_at + 1] << 8
          | (uint32_t)p[cod_at + 2] << 16;

    if (rssi_at >= 0) {
        f.rssi = (int8_t)p[rssi_at];
        f.has_rssi = true;
    }

    if (eir != 0) {
        gap_read_data(eir, eir_length, &f);
    }

    each(ctx, &f);
}

unsigned gap_decode_inquiry(const struct hci_event *e, gap_each each,
                            void *ctx)
{
    const uint8_t *p = e->params;
    unsigned n, i;

    if (e->length < 1u) {
        return 0;
    }

    n = p[0];

    switch (e->code) {
    /* BD_ADDR, Page_Scan_Repetition_Mode, two reserved, Class_Of_Device,
     * Clock_Offset: 14 bytes each. */
    case HCI_EVENT_INQUIRY_RESULT:
        if (e->length < 1u + 14u * n) {
            return 0;
        }
        for (i = 0; i < n; i++) {
            classic(p + 1 + 14u * i, 9, -1, 0, 0, each, ctx);
        }
        return n;

    /* BD_ADDR, Page_Scan_Repetition_Mode, one reserved, Class_Of_Device,
     * Clock_Offset, RSSI: 14 bytes each. */
    case HCI_EVENT_INQUIRY_RESULT_RSSI:
        if (e->length < 1u + 14u * n) {
            return 0;
        }
        for (i = 0; i < n; i++) {
            classic(p + 1 + 14u * i, 8, 13, 0, 0, each, ctx);
        }
        return n;

    /* One device, as RSSI's, and 240 bytes of EIR after it. */
    case HCI_EVENT_EXTENDED_INQUIRY:
        if (n != 1u || e->length < 15u) {
            return 0;
        }
        classic(p + 1, 8, 13, p + 15, e->length - 15u, each, ctx);
        return 1;

    default:
        return 0;
    }
}

unsigned gap_decode_advertising(const struct hci_event *e, gap_each each,
                                void *ctx)
{
    const uint8_t *p = e->params;
    unsigned n, i, at = 2;

    if (e->code != HCI_EVENT_LE_META || e->length < 2u
        || p[0] != HCI_LE_ADVERTISING_REPORT) {
        return 0;
    }

    n = p[1];

    /* Event_Type, Address_Type, Address, Length_Data, Data, RSSI. */
    for (i = 0; i < n; i++) {
        struct gap_found f;
        unsigned data_length;

        if (at + 9u > e->length) {
            return i;
        }

        data_length = p[at + 8];

        if (at + 9u + data_length + 1u > e->length) {
            return i;
        }

        memset(&f, 0, sizeof f);
        memcpy(f.address, p + at + 2, 6);
        f.transport = (p[at + 1] & 1u) ? GAP_LE_RANDOM : GAP_LE_PUBLIC;
        f.connectable = p[at] == ADV_IND || p[at] == ADV_DIRECT_IND;
        gap_read_data(p + at + 9, data_length, &f);

        /* 127 is "not available" (7.7.65.2). */
        if ((int8_t)p[at + 9 + data_length] != 127) {
            f.rssi = (int8_t)p[at + 9 + data_length];
            f.has_rssi = true;
        }

        each(ctx, &f);
        at += 9u + data_length + 1u;
    }

    return n;
}

bool gap_decode_remote_name(const struct hci_event *e, uint8_t address[6],
                            char name[GAP_NAME_BYTES])
{
    if (e->code != HCI_EVENT_REMOTE_NAME || e->length < 7u
        || e->params[0] != 0) {
        return false;
    }

    memcpy(address, e->params + 1, 6);
    gap_copy_name(name, GAP_NAME_BYTES, e->params + 7, e->length - 7u);

    return true;
}

bool gap_decode_inquiry_complete(const struct hci_event *e, uint8_t *status)
{
    if (e->code != HCI_EVENT_INQUIRY_COMPLETE || e->length < 1u) {
        return false;
    }

    *status = e->params[0];

    return true;
}

/*
 * Its kind. A Class of Device is the major class in bits 12-8 and the minor
 * in 7-2 (Assigned Numbers 2.8): a Peripheral's minor says keyboard (bit
 * 6), pointing device (bit 7) or both, and a gamepad in bits 5-2. An
 * appearance is a category in bits 15-6 and a subcategory in 5-0
 * (Assigned Numbers 2.6): Human Interface Device (0Fh) and Wearable Audio
 * Device (25h) are the ones that matter here.
 */
unsigned gap_kind(const struct gap_found *f)
{
    if (f->transport == GAP_CLASSIC) {
        unsigned major = (f->cod >> 8) & 0x1Fu;
        unsigned minor = (f->cod >> 2) & 0x3Fu;

        switch (major) {
        case 0x01: return GAP_KIND_COMPUTER;
        case 0x02: return GAP_KIND_PHONE;
        case 0x04: return GAP_KIND_HEADPHONES;
        case 0x05:
            switch (minor >> 4) {
            case 1: return GAP_KIND_KEYBOARD;
            case 2: return GAP_KIND_MOUSE;
            case 3: return GAP_KIND_KEYBOARD_MOUSE;
            default:
                return (minor & 0x0Fu) == 2u ? GAP_KIND_GAMEPAD
                                             : GAP_KIND_INPUT;
            }
        default:
            return GAP_KIND_OTHER;
        }
    }

    if (f->has_appearance) {
        unsigned category = f->appearance >> 6;
        unsigned sub = f->appearance & 0x3Fu;

        if (category == 0x0Fu) {
            switch (sub) {
            case 1: return GAP_KIND_KEYBOARD;
            case 2: return GAP_KIND_MOUSE;
            case 4: return GAP_KIND_GAMEPAD;
            default: return GAP_KIND_INPUT;
            }
        }

        if (category == 0x25u) {
            return GAP_KIND_HEADPHONES;
        }

        if (category == 0x01u) {
            return GAP_KIND_PHONE;
        }

        if (category == 0x02u) {
            return GAP_KIND_COMPUTER;
        }
    }

    return f->hid ? GAP_KIND_INPUT : GAP_KIND_OTHER;
}

const char *gap_kind_name(unsigned kind)
{
    switch (kind) {
    case GAP_KIND_KEYBOARD:       return "keyboard";
    case GAP_KIND_MOUSE:          return "mouse";
    case GAP_KIND_KEYBOARD_MOUSE: return "keyboard and mouse";
    case GAP_KIND_HEADPHONES:     return "headphones";
    case GAP_KIND_GAMEPAD:        return "game controller";
    case GAP_KIND_PHONE:          return "phone";
    case GAP_KIND_COMPUTER:       return "computer";
    case GAP_KIND_INPUT:          return "input device";
    default:                      return "other";
    }
}
