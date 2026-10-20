/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * HCI's packets, built and read. `hci_decode.h` says what and why; the
 * section numbers are the Bluetooth Core specification's, Volume 4 Part E.
 */

#include "hci_decode.h"

#include <string.h>

/* `user/include/bytes.h`, by its path from here: the host test compiles this
 * file with no include path for it. HCI is little-endian throughout (5.2). */
#include "../../include/bytes.h"

unsigned hci_command(uint8_t *out, unsigned room, uint16_t opcode,
                     const uint8_t *params, unsigned length)
{
    if (length > 255u || room < 3u + length) {
        return 0;
    }

    put_le16(out, opcode);
    out[2] = (uint8_t)length;

    if (length != 0) {
        memcpy(out + 3, params, length);
    }

    return 3u + length;
}

/*
 * The header first, two bytes; then as many parameters as its second byte
 * says. Nothing past the event's end is taken, so the rest of a packet is
 * left for the next call.
 */
bool hci_assemble(struct hci_assembler *a, const uint8_t *in, unsigned length,
                  unsigned *used)
{
    unsigned taken = 0;

    /* Whole last time: this call starts the next one. */
    if (a->have >= 2u && a->have == 2u + a->bytes[1]) {
        a->have = 0;
    }

    while (taken < length) {
        unsigned want = a->have < 2u ? 2u : 2u + a->bytes[1];

        if (a->have >= want) {
            break;
        }

        a->bytes[a->have++] = in[taken++];
    }

    *used = taken;

    return a->have >= 2u && a->have == 2u + a->bytes[1];
}

bool hci_decode_event(const uint8_t *bytes, unsigned length,
                      struct hci_event *out)
{
    if (length < 2u || length < 2u + bytes[1]) {
        return false;
    }

    out->code = bytes[0];
    out->length = bytes[1];
    out->params = bytes + 2;

    return true;
}

bool hci_decode_answer(const struct hci_event *e, struct hci_answer *out)
{
    const uint8_t *p = e->params;

    memset(out, 0, sizeof *out);

    /* Num_HCI_Command_Packets, Command_Opcode, then the return parameters,
     * whose first byte is a status for every command this reads - and for
     * all but a few (7.7.14). An opcode of 0 with nothing after it is the
     * controller saying only how many commands it will take. */
    if (e->code == HCI_EVENT_COMMAND_COMPLETE && e->length >= 3u) {
        out->complete = true;
        out->commands = p[0];
        out->opcode = get_le16(p + 1);

        if (e->length >= 4u) {
            out->status = p[3];
            out->ret = p + 4;
            out->ret_length = e->length - 4u;
        }
        return true;
    }

    /* Status, Num_HCI_Command_Packets, Command_Opcode (7.7.15). */
    if (e->code == HCI_EVENT_COMMAND_STATUS && e->length >= 4u) {
        out->status = p[0];
        out->commands = p[1];
        out->opcode = get_le16(p + 2);
        out->ret = p + 4;
        return true;
    }

    return false;
}

/* A successful Command Complete for `opcode`, with `need` bytes after the
 * status. */
static bool answered(const struct hci_answer *a, uint16_t opcode,
                     unsigned need)
{
    return a->complete && a->opcode == opcode && a->status == 0
        && a->ret != 0 && a->ret_length >= need;
}

bool hci_decode_version(const struct hci_answer *a, struct hci_version *out)
{
    if (!answered(a, HCI_READ_LOCAL_VERSION, 8u)) {
        return false;
    }

    out->hci_version = a->ret[0];
    out->hci_subversion = get_le16(a->ret + 1);
    out->lmp_version = a->ret[3];
    out->manufacturer = get_le16(a->ret + 4);
    out->lmp_subversion = get_le16(a->ret + 6);

    return true;
}

bool hci_decode_address(const struct hci_answer *a, uint8_t address[6])
{
    if (!answered(a, HCI_READ_BD_ADDR, 6u)) {
        return false;
    }

    memcpy(address, a->ret, 6);

    return true;
}

bool hci_decode_features(const struct hci_answer *a, uint8_t features[8])
{
    if (!answered(a, HCI_READ_LOCAL_FEATURES, 8u)) {
        return false;
    }

    memcpy(features, a->ret, 8);

    return true;
}

bool hci_decode_buffers(const struct hci_answer *a, struct hci_buffers *out)
{
    if (!answered(a, HCI_READ_BUFFER_SIZE, 7u)) {
        return false;
    }

    out->acl_length = get_le16(a->ret);
    out->sco_length = a->ret[2];
    out->acl_packets = get_le16(a->ret + 3);
    out->sco_packets = get_le16(a->ret + 5);

    return true;
}

bool hci_feature(const uint8_t features[8], unsigned bit)
{
    return bit < 64u && ((features[bit / 8u] >> (bit % 8u)) & 1u) != 0;
}

/* Assigned Numbers 2.1: the HCI and LMP version numbers, which are the same. */
const char *hci_version_name(uint8_t version)
{
    static const char *const names[] = {
        "1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0", "4.1", "4.2",
        "5.0", "5.1", "5.2", "5.3", "5.4", "6.0",
    };

    return version < sizeof names / sizeof names[0] ? names[version] : 0;
}

/* Assigned Numbers 7.1, the company identifiers: those that make the chips
 * in USB adapters, and a few that are only famous. */
const char *hci_manufacturer_name(uint16_t company)
{
    switch (company) {
    case 0:   return "Ericsson";
    case 1:   return "Nokia";
    case 2:   return "Intel";
    case 6:   return "Microsoft";
    case 10:  return "Qualcomm (CSR)";
    case 13:  return "Texas Instruments";
    case 15:  return "Broadcom";
    case 29:  return "Qualcomm";
    case 48:  return "STMicroelectronics";
    case 70:  return "MediaTek";
    case 76:  return "Apple";
    case 93:  return "Realtek";
    case 305: return "Cypress";
    case 0xFFFF: return "a test company";
    default:  return 0;
    }
}

void hci_format_address(const uint8_t address[6], char out[18])
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned i;

    for (i = 0; i < 6u; i++) {
        uint8_t b = address[5u - i];

        out[3u * i] = hex[b >> 4];
        out[3u * i + 1u] = hex[b & 15u];
        out[3u * i + 2u] = i < 5u ? ':' : '\0';
    }
}
