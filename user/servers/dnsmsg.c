/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A DNS message's names, header and records - `dnsmsg.h` says what this is
 * for. RFC 1035 throughout, its sections named where each rule is used.
 */

#include <stddef.h>
#include <string.h>

#include "dnsmsg.h"

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/*
 * RFC 1035 3.1: a name is labels, each its length in one byte (63 at most)
 * and then its bytes, ended by the empty label. Written from text with
 * dots; an empty label - two dots, or one at either end - is refused, since
 * it would end the name early.
 */
unsigned dns_put_name(uint8_t *out, unsigned room, const char *name, unsigned len)
{
    unsigned at = 0, start = 0, i;

    for (i = 0; i <= len; i++) {
        if (i == len || name[i] == '.') {
            unsigned label = i - start;

            if (label == 0 || label > DNS_LABEL_MAX || at + label + 1 >= room) {
                return 0;
            }

            out[at++] = (uint8_t)label;
            memcpy(out + at, name + start, label);
            at += label;
            start = i + 1;
        }
    }

    if (at + 1 > room || at + 1 > DNS_NAME_MAX) {
        return 0;
    }

    out[at++] = 0;
    return at;
}

/* RFC 1035 4.1.1: twelve bytes, six numbers. */
bool dns_read_header(const uint8_t *msg, unsigned len, struct dns_header *out)
{
    if (len < DNS_HEADER) {
        return false;
    }

    out->id          = be16(msg);
    out->flags       = be16(msg + 2);
    out->questions   = be16(msg + 4);
    out->answers     = be16(msg + 6);
    out->authorities = be16(msg + 8);
    out->additionals = be16(msg + 10);
    return true;
}

/* RFC 1035 4.1.4: a label whose two top bits are set is a pointer - the
 * other fourteen say where in the message the rest of the name is - and a
 * pointer always ends the name here. */
unsigned dns_skip_name(const uint8_t *msg, unsigned len, unsigned at)
{
    while (at < len) {
        unsigned label = msg[at];

        if (label == 0) {
            return at + 1;
        }

        if ((label & 0xc0u) == 0xc0u) {
            return at + 2 <= len ? at + 2 : len;
        }

        if ((label & 0xc0u) != 0) {
            return len;                 /* 01 and 10 are reserved: not a name */
        }

        at += label + 1;
    }

    return len;
}

/*
 * A name, pointers followed. **A pointer must point backwards**, to a name
 * already in the message before it - which is how every encoder writes
 * them, and which makes a loop impossible rather than something to count:
 * each jump goes strictly earlier, so the reading ends.
 */
bool dns_read_name(const uint8_t *msg, unsigned len, unsigned at,
                   char *out, unsigned room, unsigned *next)
{
    unsigned written = 0, end = 0, wire = 0;
    unsigned limit = at;                /* a pointer must go before this */
    bool jumped = false;

    if (room == 0) {
        return false;
    }

    for (;;) {
        unsigned label;

        if (at >= len) {
            return false;
        }

        label = msg[at];

        if (label == 0) {
            if (!jumped) {
                end = at + 1;
            }

            break;
        }

        if ((label & 0xc0u) == 0xc0u) {
            unsigned to;

            if (at + 2 > len) {
                return false;
            }

            to = ((label & 0x3fu) << 8) | msg[at + 1];

            if (to >= limit) {
                return false;           /* forward, or onto itself: a loop */
            }

            if (!jumped) {
                end = at + 2;
                jumped = true;
            }

            limit = to;
            at = to;
            continue;
        }

        if ((label & 0xc0u) != 0 || at + 1 + label > len) {
            return false;
        }

        wire += label + 1;

        if (wire + 1 > DNS_NAME_MAX) {
            return false;               /* RFC 1035 2.3.4: 255 at most */
        }

        if (written != 0) {
            if (written + 1 >= room) {
                return false;
            }

            out[written++] = '.';
        }

        if (written + label >= room) {
            return false;
        }

        memcpy(out + written, msg + at + 1, label);
        written += label;
        at += label + 1;
    }

    out[written] = '\0';

    if (next != NULL) {
        *next = end;
    }

    return true;
}

/* RFC 1035 4.1.2: a name, then its type and class. */
bool dns_read_question(const uint8_t *msg, unsigned len, unsigned *at,
                       char *name, unsigned room, uint16_t *type, uint16_t *class)
{
    unsigned after;

    if (!dns_read_name(msg, len, *at, name, room, &after) || after + 4 > len) {
        return false;
    }

    *type  = be16(msg + after);
    *class = be16(msg + after + 2);
    *at    = after + 4;
    return true;
}

/* RFC 1035 4.1.3: a name, its type, class, time to live, and its data's
 * length and the data - which must lie inside the message. */
bool dns_read_record(const uint8_t *msg, unsigned len, unsigned *at,
                     struct dns_record *out)
{
    unsigned after;

    if (!dns_read_name(msg, len, *at, out->name, sizeof(out->name), &after)
        || after + 10 > len) {
        return false;
    }

    out->type   = be16(msg + after);
    out->class  = be16(msg + after + 2);
    out->ttl    = ((uint32_t)be16(msg + after + 4) << 16) | be16(msg + after + 6);
    out->length = be16(msg + after + 8);
    out->data   = after + 10;

    if (out->data + out->length > len) {
        return false;
    }

    *at = out->data + out->length;
    return true;
}
