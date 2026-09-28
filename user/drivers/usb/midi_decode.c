/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A USB MIDI device's bytes, decoded. `midi_decode.h` says what and why;
 * the section numbers are USB MIDI 1.0's.
 */

#include "midi_decode.h"

#include <string.h>

/* Descriptor types (USB 2.0 9.4, USB Audio 1.0 A.4). */
#define DESC_CONFIGURATION  0x02
#define DESC_INTERFACE      0x04
#define DESC_ENDPOINT       0x05
#define DESC_CS_INTERFACE   0x24
#define DESC_CS_ENDPOINT    0x25

/* The audio class and its MIDIStreaming subclass (USB Audio 1.0 A.1, A.2). */
#define CLASS_AUDIO         0x01
#define SUBCLASS_MIDI       0x03

/* MIDIStreaming descriptor subtypes (USB MIDI 1.0 A.1, A.2). */
#define MIDI_IN_JACK        0x02
#define MIDI_OUT_JACK       0x03
#define MS_GENERAL          0x01

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/*
 * The walk. A MIDIStreaming interface declares its jacks (6.1.2.2, 6.1.2.3)
 * and then its endpoints, each followed by a class descriptor listing the
 * embedded jacks it carries (6.2.2): on the IN endpoint those are OUT jacks
 * - data leaving the device's MIDI function towards the host - and on the
 * OUT endpoint IN jacks. The order of that list is the cable numbering.
 *
 * Names are a jack's `iJack`, which comes before the endpoint that names
 * the jack, so the jacks are kept by ID and the ports resolved at the end.
 */
void midi_decode_config(const uint8_t *bytes, unsigned length,
                        struct usb_midi *out)
{
    uint8_t jack_name[256];
    uint8_t in_jacks[MIDI_CABLES], out_jacks[MIDI_CABLES];
    unsigned n_in = 0, n_out = 0;
    bool in_midi = false;           /* inside a MIDIStreaming setting */
    bool taken = false;             /* one with an endpoint was found */
    bool found = false;             /* any MIDIStreaming interface at all */
    int last = 0;                   /* 1: the IN endpoint just read, 2: OUT */
    unsigned at = 0;

    memset(out, 0, sizeof *out);
    memset(jack_name, 0, sizeof jack_name);

    while (at < length) {
        /* A byte left over is a descriptor cut short too, not an end. */
        if (at + 2 > length || bytes[at] < 2 || at + bytes[at] > length) {
            out->malformed = true;
            break;
        }

        unsigned len = bytes[at];
        uint8_t type = bytes[at + 1];
        const uint8_t *d = bytes + at;

        switch (type) {
        case DESC_CONFIGURATION:
            if (len >= 6) out->configuration = d[5];
            break;

        case DESC_INTERFACE:
            last = 0;

            if (len < 9) break;

            /* Leaving a setting that gave an endpoint: that is the one. */
            if (in_midi && (out->in_endpoint || out->out_endpoint)) taken = true;

            in_midi = !taken && d[5] == CLASS_AUDIO && d[6] == SUBCLASS_MIDI;

            if (in_midi) {
                found = true;
                out->interface = d[2];
                out->alternate = d[3];
                out->name = d[8];
                n_in = n_out = 0;
            }
            break;

        case DESC_CS_INTERFACE:
            if (!in_midi || len < 3) break;

            if (d[2] == MIDI_IN_JACK && len >= 6) {
                jack_name[d[4]] = d[5];
            } else if (d[2] == MIDI_OUT_JACK && len >= 7) {
                unsigned pins = d[5];

                if (7 + 2 * pins <= len) jack_name[d[4]] = d[6 + 2 * pins];
            }
            break;

        case DESC_ENDPOINT:
            last = 0;

            if (!in_midi || len < 7 || (d[3] & 3) != 2 || (d[2] & 0x0F) == 0) break;

            if (d[2] & 0x80) {
                if (!out->in_endpoint) {
                    out->in_endpoint = d[2] & 0x0F;
                    out->in_packet = le16(d + 4) & 0x7FF;
                    last = 1;
                }
            } else if (!out->out_endpoint) {
                out->out_endpoint = d[2] & 0x0F;
                out->out_packet = le16(d + 4) & 0x7FF;
                last = 2;
            }
            break;

        case DESC_CS_ENDPOINT:
            if (in_midi && last && len >= 4 && d[2] == MS_GENERAL) {
                unsigned n = d[3];

                if (4 + n > len) n = len - 4;
                if (n > MIDI_CABLES) n = MIDI_CABLES;

                for (unsigned i = 0; i < n; i++) {
                    if (last == 1) in_jacks[i] = d[4 + i];
                    else out_jacks[i] = d[4 + i];
                }

                if (last == 1) n_in = n;
                else n_out = n;
            }

            last = 0;
            break;

        default:
            break;
        }

        at += len;
    }

    /*
     * The ports. An endpoint with no jacks listed still carries cable 0:
     * the listing is the specification's and not every device keeps it.
     */
    if (out->in_endpoint) {
        out->nin = n_in ? n_in : 1;

        for (unsigned i = 0; i < out->nin; i++) {
            uint8_t jack = n_in ? in_jacks[i] : 0;

            out->in[i] = (struct midi_port){ (uint8_t)i, jack, jack_name[jack] };
        }
    }

    if (out->out_endpoint) {
        out->nout = n_out ? n_out : 1;

        for (unsigned i = 0; i < out->nout; i++) {
            uint8_t jack = n_out ? out_jacks[i] : 0;

            out->out[i] = (struct midi_port){ (uint8_t)i, jack, jack_name[jack] };
        }
    }

    out->ok = found && (out->in_endpoint || out->out_endpoint);
}

/*
 * How many MIDI bytes a packet carries, by its Code Index Number (4,
 * table 4-1). 0 and 1 are reserved - and 0 is what padding looks like.
 */
static const uint8_t CIN_BYTES[16] = {
    0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1,
};

bool midi_decode_packet(const uint8_t packet[4], struct midi_event *out)
{
    uint8_t cin = packet[0] & 0x0F;
    uint8_t first = packet[1];

    if (CIN_BYTES[cin] == 0) return false;

    out->cable = packet[0] >> 4;
    out->length = CIN_BYTES[cin];
    out->bytes[0] = packet[1];
    out->bytes[1] = out->length > 1 ? packet[2] : 0;
    out->bytes[2] = out->length > 2 ? packet[3] : 0;

    /*
     * 4 starts or continues a System Exclusive message, 6 and 7 end one;
     * 5 is either one byte of System Common (F1 to F6) or the end of one,
     * and the byte says which.
     */
    out->sysex = cin == 4 || cin == 6 || cin == 7
              || (cin == 5 && !(first >= 0xF1 && first <= 0xF6));

    return true;
}

unsigned midi_message_length(uint8_t status)
{
    if (status < 0x80) return 0;
    if (status < 0xC0) return 3;            /* note off, on, pressure, controller */
    if (status < 0xE0) return 2;            /* program, channel pressure */
    if (status < 0xF0) return 3;            /* pitch bend */

    switch (status) {
    case 0xF1: case 0xF3: return 2;         /* time code, song select */
    case 0xF2: return 3;                    /* song position */
    case 0xF6: return 1;                    /* tune request */
    case 0xF0: case 0xF4: case 0xF5: case 0xF7: return 0;
    default: return 1;                      /* F8 to FF: real time */
    }
}

unsigned midi_encode(uint8_t cable, const uint8_t *message, unsigned length,
                     uint8_t (*out)[4], unsigned room)
{
    uint8_t c = (uint8_t)((cable & 0x0F) << 4);

    if (length == 0 || message[0] < 0x80) return 0;

    /* System Exclusive: three bytes a packet, the last saying how many. */
    if (message[0] == 0xF0) {
        unsigned end = 0;

        while (end < length && message[end] != 0xF7) end++;

        if (end == length) return 0;

        unsigned n = end + 1;
        unsigned packets = (n + 2) / 3;

        if (packets > room) return 0;

        for (unsigned p = 0; p < packets; p++) {
            unsigned from = p * 3;
            unsigned left = n - from;
            unsigned here = left > 3 ? 3 : left;
            uint8_t cin = (p + 1 < packets) ? 0x4 : (uint8_t)(0x4 + here);

            out[p][0] = (uint8_t)(c | cin);
            out[p][1] = message[from];
            out[p][2] = here > 1 ? message[from + 1] : 0;
            out[p][3] = here > 2 ? message[from + 2] : 0;
        }

        return packets;
    }

    unsigned n = midi_message_length(message[0]);

    if (n == 0 || length < n || room < 1) return 0;

    for (unsigned i = 1; i < n; i++) {
        if (message[i] >= 0x80) return 0;
    }

    uint8_t cin;

    if (message[0] < 0xF0) {
        cin = message[0] >> 4;
    } else if (message[0] >= 0xF8) {
        cin = 0xF;
    } else {
        cin = n == 1 ? 0x5 : (n == 2 ? 0x2 : 0x3);
    }

    out[0][0] = (uint8_t)(c | cin);
    out[0][1] = message[0];
    out[0][2] = n > 1 ? message[1] : 0;
    out[0][3] = n > 2 ? message[2] : 0;
    return 1;
}
