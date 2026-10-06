/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What a server calls itself, out of NTLM's challenge: `ntlm_name.h` says
 * which of its three names is taken and why (`testing.md` 18.415).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ntlm_name.h"

#define CHALLENGE_TYPE      2u
#define CHALLENGE_FIXED    48u     /* through TargetInfo's fields */
#define AV_EOL              0u
#define AV_NB_COMPUTER      1u
#define AV_DNS_COMPUTER     3u

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

/* A field's bytes - two bytes of length, two of room, four of offset - or
 * false when they run past what there is. An empty field is there. */
static bool field(const uint8_t *msg, size_t len, size_t at,
                  const uint8_t **out, size_t *bytes)
{
    size_t n = le16(msg + at);
    size_t off = le32(msg + at + 4);

    if (n == 0) {
        *out = NULL;
        *bytes = 0;
        return true;
    }

    if (off > len || n > len - off) {
        return false;
    }

    *out = msg + off;
    *bytes = n;
    return true;
}

/*
 * UTF-16LE as UTF-8, `stop` code units at most (a DNS name's first label
 * stops at its first dot). A lone surrogate, or a control character, makes
 * it no name at all: a server's name is shown to a person and put in a
 * path. Returns the bytes written, 0 for none.
 */
static size_t utf8_of(const uint8_t *u, size_t units, bool first_label,
                      char *out, size_t room)
{
    size_t at = 0, i;

    if (room == 0) {
        return 0;
    }

    for (i = 0; i < units; i++) {
        uint32_t c = le16(u + 2 * i);
        char enc[4];
        size_t n;

        if (c >= 0xd800 && c <= 0xdbff) {
            uint32_t low = (i + 1 < units) ? le16(u + 2 * (i + 1)) : 0;

            if (low < 0xdc00 || low > 0xdfff) {
                return 0;
            }

            c = 0x10000u + ((c - 0xd800u) << 10) + (low - 0xdc00u);
            i++;
        } else if (c >= 0xdc00 && c <= 0xdfff) {
            return 0;
        }

        if (first_label && c == '.') {
            break;
        }

        if (c < 0x20 || c == 0x7f || c == '/') {
            return 0;
        }

        if (c < 0x80) {
            enc[0] = (char)c;
            n = 1;
        } else if (c < 0x800) {
            enc[0] = (char)(0xc0 | (c >> 6));
            enc[1] = (char)(0x80 | (c & 0x3f));
            n = 2;
        } else if (c < 0x10000) {
            enc[0] = (char)(0xe0 | (c >> 12));
            enc[1] = (char)(0x80 | ((c >> 6) & 0x3f));
            enc[2] = (char)(0x80 | (c & 0x3f));
            n = 3;
        } else {
            enc[0] = (char)(0xf0 | (c >> 18));
            enc[1] = (char)(0x80 | ((c >> 12) & 0x3f));
            enc[2] = (char)(0x80 | ((c >> 6) & 0x3f));
            enc[3] = (char)(0x80 | (c & 0x3f));
            n = 4;
        }

        if (at + n > room - 1) {
            break;                      /* cut at a character */
        }

        for (size_t k = 0; k < n; k++) {
            out[at++] = enc[k];
        }
    }

    out[at] = '\0';
    return at;
}

/* Only digits and dots - an address, or a piece of one - is no name. */
static bool is_a_name(const char *s, size_t n)
{
    size_t i;

    if (n == 0) {
        return false;
    }

    for (i = 0; i < n; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) {
            return true;
        }
    }

    return false;
}

static bool take(const uint8_t *u, size_t bytes, bool first_label,
                 char *out, size_t room)
{
    size_t n = utf8_of(u, bytes / 2, first_label, out, room);

    if (is_a_name(out, n)) {
        return true;
    }

    if (room > 0) {
        out[0] = '\0';
    }

    return false;
}

int ntlm_challenge_name(const uint8_t *msg, size_t len, char *out, size_t room)
{
    static const uint8_t SIGNATURE[8] = { 'N', 'T', 'L', 'M', 'S', 'S', 'P', 0 };
    const uint8_t *target = NULL, *info = NULL, *nb = NULL, *dns = NULL;
    size_t target_bytes = 0, info_bytes = 0, nb_bytes = 0, dns_bytes = 0;
    size_t i;

    if (room > 0) {
        out[0] = '\0';
    }

    for (i = 0; i < 8 && i < len; i++) {
        if (msg[i] != SIGNATURE[i]) {
            return NTLM_NAME_NOT;
        }
    }

    if (len < 12) {
        return NTLM_NAME_SHORT;
    }

    if (le32(msg + 8) != CHALLENGE_TYPE) {
        return NTLM_NAME_NOT;
    }

    if (len < CHALLENGE_FIXED) {
        return NTLM_NAME_SHORT;
    }

    if (!field(msg, len, 12, &target, &target_bytes)
        || !field(msg, len, 40, &info, &info_bytes)) {
        return NTLM_NAME_SHORT;
    }

    /* TargetInfo's pairs: an id, a length, the value; to MsvAvEOL. */
    for (i = 0; info != NULL && i + 4 <= info_bytes; ) {
        uint16_t id = le16(info + i);
        size_t n = le16(info + i + 2);

        if (id == AV_EOL || n > info_bytes - i - 4) {
            break;
        }

        if (id == AV_NB_COMPUTER && nb == NULL) {
            nb = info + i + 4;
            nb_bytes = n;
        } else if (id == AV_DNS_COMPUTER && dns == NULL) {
            dns = info + i + 4;
            dns_bytes = n;
        }

        i += 4 + n;
    }

    if ((nb != NULL && take(nb, nb_bytes, false, out, room))
        || (dns != NULL && take(dns, dns_bytes, true, out, room))
        || (target != NULL && take(target, target_bytes, false, out, room))) {
        return NTLM_NAME_FOUND;
    }

    return NTLM_NAME_NONE;
}
