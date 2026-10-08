/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Mail Kit's reading of a message (`docs/mail.md` M1): RFC 5322's
 * header fields, MIME's parts (RFC 2045, 2046), encoded words (RFC 2047)
 * and parameters (RFC 2231), base64 and quoted-printable undone, and a
 * text part's characters made UTF-8. Plain C over bytes it is handed and
 * does not keep; `mail_kosmos.c` is its Lua door, and `tools/test_mail.c`
 * holds it to messages made for it, on the Mac.
 *
 * **A message is parsed once** into a table of parts, each pointing into
 * the bytes; nothing is copied until a part or a field is asked for.
 */

#ifndef KOSMOS_MIME_H
#define KOSMOS_MIME_H

#include <stddef.h>
#include <stdint.h>

#define MIME_PARTS_MOST   256u      /* parts a message may have; past that, ignored */
#define MIME_DEPTH_MOST    16u      /* multiparts inside multiparts */

struct mime_part {
    int      parent;            /* its multipart's index; -1 for the message */
    int      depth;
    size_t   head, head_len;    /* its header block, in the message's bytes */
    size_t   body, body_len;    /* its body, as it is in the message */
    char     type[64];          /* "text/plain", lower case */
    char     charset[40];       /* lower case; "" for none said */
    char     encoding[24];      /* "base64", "quoted-printable", "7bit"... lower case */
    char     name[160];         /* a file's name, UTF-8; "" for none */
    char     disposition[16];   /* "attachment", "inline", or "" */
    char     cid[128];          /* Content-ID, without its < > */
    int      multipart;         /* 1 for a multipart, whose body is its children */
};

struct mime_msg {
    const uint8_t *buf;
    size_t   len;
    size_t   nparts;            /* part[0] is the message itself */
    struct mime_part part[MIME_PARTS_MOST];
};

struct mime_address {
    char     name[128];         /* UTF-8; "" when only the address was given */
    char     address[256];
};

/* A message's bytes, parsed; 0, or -1 when there is no header at all. The
 * bytes are kept by the caller for as long as `m` is used. */
int mime_parse(struct mime_msg *m, const uint8_t *buf, size_t len);

/* Part `p`'s field `name`, unfolded and its encoded words decoded, in UTF-8,
 * into `out`; its length, or -1 when the part has no such field. */
long mime_header(const struct mime_msg *m, size_t p, const char *name, char *out, size_t room);

/* Part `p`'s content: its transfer encoding undone, and a text part's
 * characters in UTF-8. How many bytes; never more than `room`. */
size_t mime_part_bytes(const struct mime_msg *m, size_t p, uint8_t *out, size_t room);

/* The most `mime_part_bytes` can make of part `p`: room enough for it. */
size_t mime_part_bound(const struct mime_msg *m, size_t p);

/* A date as RFC 5322 writes it - "Tue, 6 Oct 2026 09:41:07 +0200" - in
 * seconds since 1970 in UTC, its zone applied; `*ok` 0 when it is not one. */
int64_t mime_date(const char *value, int *ok);

/* The addresses in a From, To or Cc field's value (already decoded):
 * "Lena Moreau <lena@example.com>, bob@example.org"; how many. */
size_t mime_addresses(const char *value, struct mime_address *out, size_t most);

/* The first words of a message's text - its first plain text part, or its
 * HTML with the tags taken out - with runs of space made one and quoted
 * lines left out, at most `room - 1` bytes of UTF-8 cut on a character. */
size_t mime_preview(const struct mime_msg *m, char *out, size_t room);

/* Bytes in `charset` made UTF-8 - UTF-8 and US-ASCII as they are (bad
 * sequences replaced), ISO-8859-1 to -16, Windows-1250 to -1258; a charset
 * not known is taken as UTF-8. How many bytes; never more than `room`. */
size_t mime_to_utf8(const char *charset, const uint8_t *in, size_t n, uint8_t *out, size_t room);

#endif
