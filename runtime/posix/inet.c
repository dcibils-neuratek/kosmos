/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Addresses as text** - `inet_pton`, `inet_ntop` and the old IPv4 forms
 * beside them - for ported code (`docs/w3.md`).
 *
 * Apart from the sockets because they touch nothing: no network, no
 * descriptor, no state, only text and bytes. That is what lets
 * `tools/test_inet.c` compile this file on the Mac and hold every answer
 * to the Mac's own.
 *
 * **IPv6 is here although the network stack has none.** Text in IPv6's
 * form is still text a program asks about, and its answer is a fact about
 * the text: WebCore asks `inet_pton(AF_INET6, host)` of every URL's host
 * before it asks AF_INET, and took the -1 this file used to give - "no such
 * family" - for "yes", since a Unix where IPv6 is missing does not exist.
 * So every host became an IPv6 address made of whatever the stack held,
 * and a page that loaded an image died when those bytes were all zero
 * (`testing.md` 18.797). The forms are RFC 4291's, written back as RFC
 * 5952 says, as BSD's and glibc's do.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

/* Four numbers and three dots, each under 256, and nothing after them. */
static int four_numbers(const char *text, uint8_t out[4])
{
    unsigned part = 0, value = 0, digits = 0;

    for (;; text++) {
        if (*text >= '0' && *text <= '9') {
            value = value * 10u + (unsigned)(*text - '0');

            if (++digits > 3 || value > 255u) {
                return 0;
            }
        } else if ((*text == '.' || *text == '\0') && digits > 0 && part < 4) {
            out[part++] = (uint8_t)value;
            value = digits = 0;

            if (*text == '\0') {
                return part == 4;
            }
        } else {
            return 0;
        }
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

/*
 * Eight groups of up to four hex digits between colons; one `::` standing
 * for as many zero groups as are missing; and the last two groups may be
 * written as an IPv4 address instead. Nothing else - no zone after a `%`,
 * no brackets, which belong to a URL rather than to an address.
 */
static int sixteen_bytes(const char *text, uint8_t out[16])
{
    uint8_t b[16] = { 0 };
    int at = 0;              /* the next byte to fill */
    int gap = -1;            /* where `::` stood, as a byte offset */

    if (text[0] == ':') {
        if (text[1] != ':') {
            return 0;
        }

        text++;              /* the second colon is read as a separator */
    }

    while (*text != '\0') {
        if (*text == ':') {
            if (gap >= 0) {
                return 0;    /* a second `::` */
            }

            gap = at;
            text++;
            continue;
        }

        const char *group = text;
        unsigned value = 0;
        int digits = 0, d;

        while ((d = hex_digit(*text)) >= 0) {
            if (++digits > 4) {
                return 0;
            }

            value = value << 4 | (unsigned)d;
            text++;
        }

        if (*text == '.') {
            /* An IPv4 address as the last 32 bits, and the end. */
            if (at > 12 || !four_numbers(group, b + at)) {
                return 0;
            }

            at += 4;
            break;
        }

        if (digits == 0 || at > 14) {
            return 0;
        }

        b[at++] = (uint8_t)(value >> 8);
        b[at++] = (uint8_t)value;

        if (*text == ':') {
            text++;

            if (*text == '\0') {
                return 0;    /* one colon at the end */
            }
        } else if (*text != '\0') {
            return 0;
        }
    }

    if (gap >= 0) {
        int moved = at - gap;

        if (at == 16) {
            return 0;        /* `::` that stands for nothing */
        }

        memmove(b + 16 - moved, b + gap, (size_t)moved);
        memset(b + gap, 0, (size_t)(16 - moved - gap));
    } else if (at != 16) {
        return 0;
    }

    memcpy(out, b, 16);
    return 1;
}

int inet_pton(int family, const char *text, void *out)
{
    if (family == AF_INET) {
        return text != NULL && four_numbers(text, out) ? 1 : 0;
    }

    if (family == AF_INET6) {
        return text != NULL && sixteen_bytes(text, out) ? 1 : 0;
    }

    errno = EAFNOSUPPORT;
    return -1;
}

/*
 * RFC 5952: lowercase, no leading zeros, and the longest run of two or
 * more zero groups as `::` - the first, of two as long. An address that is
 * an IPv4 one in IPv6's clothes - `::ffff:a.b.c.d`, or the older
 * `::a.b.c.d` - keeps its dots, as BSD's writes it.
 */
static int write_six(const uint8_t *b, char *text)
{
    unsigned words[8];
    int best = -1, best_len = 0, run = -1, i;
    char *t = text;

    for (i = 0; i < 8; i++) {
        words[i] = (unsigned)b[2 * i] << 8 | b[2 * i + 1];
    }

    for (i = 0; i <= 8; i++) {
        if (i < 8 && words[i] == 0) {
            if (run < 0) {
                run = i;
            }
        } else if (run >= 0) {
            if (i - run > best_len) {
                best = run;
                best_len = i - run;
            }

            run = -1;
        }
    }

    if (best_len < 2) {
        best = -1;
    }

    for (i = 0; i < 8; i++) {
        if (best >= 0 && i >= best && i < best + best_len) {
            if (i == best) {
                *t++ = ':';
                if (i == 0) {
                    *t++ = ':';
                }
            }
            continue;
        }

        if (i == 6 && best == 0
            && (best_len == 6 || (best_len == 5 && words[5] == 0xffff))) {
            t += sprintf(t, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
            return (int)(t - text);
        }

        t += sprintf(t, "%x", words[i]);

        if (i < 7) {
            *t++ = ':';
        }
    }

    *t = '\0';
    return (int)(t - text);
}

const char *inet_ntop(int family, const void *address, char *out, socklen_t room)
{
    const uint8_t *b = address;
    char text[INET6_ADDRSTRLEN];
    int n;

    if (family == AF_INET) {
        n = snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    } else if (family == AF_INET6) {
        n = write_six(b, text);
    } else {
        errno = EAFNOSUPPORT;
        return NULL;
    }

    if (out == NULL || n < 0 || (socklen_t)n >= room) {
        errno = ENOSPC;
        return NULL;
    }

    memcpy(out, text, (size_t)n + 1);
    return out;
}

int inet_aton(const char *text, struct in_addr *out)
{
    uint8_t b[4];

    if (text == NULL || !four_numbers(text, b)) {
        return 0;
    }

    if (out != NULL) {
        memcpy(&out->s_addr, b, 4);
    }

    return 1;
}

in_addr_t inet_addr(const char *text)
{
    struct in_addr a;

    return inet_aton(text, &a) ? a.s_addr : INADDR_NONE;
}

char *inet_ntoa(struct in_addr address)
{
    static char text[INET_ADDRSTRLEN];      /* POSIX's: one, and not a thread's */

    return (char *)inet_ntop(AF_INET, &address.s_addr, text, sizeof(text));
}
