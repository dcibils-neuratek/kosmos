/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Mail Kit's writing (`docs/mail.md` M6): a message made from what a
 * person wrote - RFC 5322's header, its words encoded by RFC 2047 where
 * they are not ASCII, and a plain-text body in UTF-8, sent as it is when it
 * is ASCII in short lines and quoted-printable when it is not. The reading
 * half is `mime.c`; `tools/test_mail.c` holds this one to the byte and reads
 * every message it makes back through the other.
 *
 * **With attachments** (M7) it is `multipart/mixed`: the text first, then
 * each file in base64 in lines of 76, a name that is not ASCII written as
 * RFC 2231 has it.
 * The boundary begins `=_`, which quoted-printable never writes, so the
 * text is sent quoted-printable whenever there are files and no line of it
 * can be mistaken for the boundary.
 *
 * A Bcc is in no header - `smtp.lua` gives its addresses to the server -
 * and a line beginning with a dot is left as it is: doubling it is the
 * SMTP conversation's business, not the message's.
 *
 * Every write goes through `put`, which counts what does not fit, so one
 * pass says both what the message is and how much room it wants.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mime.h"
#include "kits/compress/base64_core.h"

#define LINE_MOST 76u           /* a header's line or a body's, folded or broken here */
#define WORD_BYTES 45u          /* UTF-8 an encoded word carries: 60 of base64, 75 in all */

struct out {
    uint8_t *at;
    size_t   room, n;
    size_t   col;               /* characters since the last line end */
};

static void put(struct out *o, const void *bytes, size_t len)
{
    const uint8_t *b = bytes;

    for (size_t i = 0; i < len; i++) {
        if (o->n < o->room) o->at[o->n] = b[i];
        o->n++;
        o->col = b[i] == '\n' ? 0 : o->col + 1;
    }
}

static void puts_(struct out *o, const char *s)
{
    put(o, s, strlen(s));
}

static void crlf(struct out *o)
{
    put(o, "\r\n", 2);
}

/* A header's line folded: a line end and the space that continues it. */
static void fold(struct out *o)
{
    put(o, "\r\n ", 3);
}

static int ascii(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];

        if (c >= 0x80 || (c < 0x20 && c != '\t')) return 0;
    }

    return 1;
}

/* How many bytes of `s` make up whole characters within `most`. */
static size_t whole_chars(const char *s, size_t len, size_t most)
{
    size_t n = len < most ? len : most;

    if (n == len) return n;

    /* Back off a character's continuation bytes, so none is split. */
    while (n > 0 && ((uint8_t)s[n] & 0xc0) == 0x80) n--;
    return n;
}

/*
 * `s` as encoded words, `=?UTF-8?B?...?=`, each carrying whole characters;
 * words after the first on lines of their own when the line would be long.
 */
static void encoded_words(struct out *o, const char *s, size_t len)
{
    size_t i = 0;
    int first = 1;

    while (i < len) {
        size_t lead = first ? 0 : 1;
        size_t fits, take, k;
        char b64[WORD_BYTES / 3 * 4 + 8];

        /* As much as the line has room for, in whole characters; a new
         * line when it has room for less than one of four bytes. */
        fits = o->col + lead + 12 + 8 <= LINE_MOST ? (LINE_MOST - o->col - lead - 12) / 4 * 3 : 0;
        if (fits > WORD_BYTES) fits = WORD_BYTES;
        take = whole_chars(s + i, len - i, fits);

        if (take == 0 || fits < 4) {
            fold(o);
            lead = 0;
            take = whole_chars(s + i, len - i, WORD_BYTES);
        } else if (!first) {
            put(o, " ", 1);
        }

        if (take == 0) take = 1;        /* a byte that begins nothing: carried alone */
        k = base64_encode((const uint8_t *)s + i, take, b64);

        puts_(o, "=?UTF-8?B?");
        put(o, b64, k);
        puts_(o, "?=");
        i += take;
        first = 0;
    }
}

/* ASCII words, folded at a space when the line would pass its length. */
static void plain_words(struct out *o, const char *s, size_t len)
{
    size_t i = 0;
    int first = 1;

    while (i < len) {
        size_t j;

        while (i < len && s[i] == ' ') i++;
        j = i;
        while (j < len && s[j] != ' ') j++;
        if (j == i) break;

        if (!first && o->col + 1 + (j - i) > LINE_MOST) fold(o);
        else if (!first) put(o, " ", 1);

        put(o, s + i, j - i);
        i = j;
        first = 0;
    }
}

size_t mail_encode_words(const char *text, char *out, size_t room)
{
    struct out o = { (uint8_t *)out, room, 0, 0 };
    size_t len = strlen(text);

    if (ascii(text, len)) plain_words(&o, text, len);
    else encoded_words(&o, text, len);

    return o.n;
}

/* A display name as RFC 5322 allows it: as it is, quoted, or encoded. */
static void display_name(struct out *o, const char *name)
{
    size_t len = strlen(name);
    int special = 0;

    if (!ascii(name, len)) {
        encoded_words(o, name, len);
        return;
    }

    for (size_t i = 0; i < len; i++) {
        if (strchr("()<>[]:;@\\,.\"", name[i])) special = 1;
    }

    if (!special) {
        put(o, name, len);
        return;
    }

    put(o, "\"", 1);

    for (size_t i = 0; i < len; i++) {
        if (name[i] == '"' || name[i] == '\\') put(o, "\\", 1);
        put(o, name + i, 1);
    }

    put(o, "\"", 1);
}

static void address_field(struct out *o, const char *field, const struct mime_address *a, size_t n)
{
    puts_(o, field);
    puts_(o, ": ");

    for (size_t i = 0; i < n; i++) {
        size_t want = strlen(a[i].name) + strlen(a[i].address) + 4;

        if (i > 0) {
            put(o, ",", 1);
            if (o->col + 1 + want > LINE_MOST) fold(o);
            else put(o, " ", 1);
        }

        if (a[i].name[0]) {
            display_name(o, a[i].name);
            put(o, " <", 2);
            puts_(o, a[i].address);
            put(o, ">", 1);
        } else {
            puts_(o, a[i].address);
        }
    }

    crlf(o);
}

/* `t` seconds since 1970 in UTC, written in a zone `zone` minutes east. */
static void date_field(struct out *o, int64_t t, int zone)
{
    static const char *const days[] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int64_t local = t + (int64_t)zone * 60;
    int64_t day = local >= 0 ? local / 86400 : (local - 86399) / 86400;
    int64_t secs = local - day * 86400;
    /* Civil from days (Howard Hinnant's algorithm). */
    int64_t z = day + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    int64_t y = yoe + era * 400 + (m <= 2);
    int az = zone < 0 ? -zone : zone;
    char s[64];
    char *p = s;
    long wd = (long)(((day % 7) + 7) % 7);
    long parts[] = { (long)(secs / 3600), (long)(secs / 60 % 60), (long)(secs % 60) };

    puts_(o, "Date: ");
    puts_(o, days[wd]);
    puts_(o, ", ");

    /* Numbers by hand: this is freestanding C, with no printf. */
    if (d >= 10) *p++ = (char)('0' + d / 10);
    *p++ = (char)('0' + d % 10);
    *p++ = ' ';
    memcpy(p, months[m - 1], 3);
    p += 3;
    *p++ = ' ';
    p[0] = (char)('0' + y / 1000 % 10);
    p[1] = (char)('0' + y / 100 % 10);
    p[2] = (char)('0' + y / 10 % 10);
    p[3] = (char)('0' + y % 10);
    p += 4;

    for (int i = 0; i < 3; i++) {
        *p++ = i == 0 ? ' ' : ':';
        *p++ = (char)('0' + parts[i] / 10);
        *p++ = (char)('0' + parts[i] % 10);
    }

    *p++ = ' ';
    *p++ = zone < 0 ? '-' : '+';
    *p++ = (char)('0' + az / 600 % 10);
    *p++ = (char)('0' + az / 60 % 10);
    *p++ = (char)('0' + az % 60 / 10);
    *p++ = (char)('0' + az % 10);
    put(o, s, (size_t)(p - s));
    crlf(o);
}

/* Is the text fine as it is: ASCII, no control characters, short lines? */
static int seven_bit(const uint8_t *s, size_t len)
{
    size_t col = 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = s[i];

        if (c == '\n') {
            col = 0;
            continue;
        }

        if (c == '\r') continue;
        if (c >= 0x80 || (c < 0x20 && c != '\t')) return 0;
        if (++col > LINE_MOST) return 0;
    }

    return 1;
}

/* One line of text as quoted-printable (RFC 2045 6.7), with soft breaks;
 * carriage returns in it are dropped. */
static void qp_line(struct out *o, const uint8_t *s, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t col = 0;
    size_t end = len;

    while (end > 0 && s[end - 1] == '\r') end--;

    for (size_t i = 0; i < end; i++) {
        uint8_t c = s[i];
        int last = i + 1 == end;
        char e[3];
        size_t w;

        if (c == '\r') continue;

        if ((c >= 33 && c <= 126 && c != '=') || ((c == ' ' || c == '\t') && !last)) {
            e[0] = (char)c;
            w = 1;
        } else {
            e[0] = '=';
            e[1] = hex[c >> 4];
            e[2] = hex[c & 15];
            w = 3;
        }

        /* Room for the character and, unless it ends the line, a soft break. */
        if (col + w > (last ? LINE_MOST : LINE_MOST - 1)) {
            put(o, "=\r\n", 3);
            col = 0;
        }

        put(o, e, w);
        col += w;
    }

    crlf(o);
}

static void plain_line(struct out *o, const uint8_t *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] != '\r') put(o, s + i, 1);
    }

    crlf(o);
}

/* The body's lines, ended by LF or CRLF, each written by `line`; a last
 * line with no end is a line too, and an empty text one empty line. */
static void body_lines(struct out *o, const uint8_t *s, size_t len,
                       void (*line)(struct out *, const uint8_t *, size_t))
{
    size_t i = 0;

    do {
        size_t j = i;

        while (j < len && s[j] != '\n') j++;
        line(o, s + i, j - i);
        i = j + 1;
    } while (i < len);
}

/* A parameter's value: quoted when it is plain ASCII, and otherwise as
 * RFC 2231 has it - `key*=UTF-8''caf%C3%A9.txt`. */
static void name_param(struct out *o, const char *key, const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t len = strlen(value);
    int plain = ascii(value, len);

    for (size_t i = 0; plain && i < len; i++) {
        if (value[i] == '"' || value[i] == '\\') plain = 0;
    }

    put(o, ";", 1);
    fold(o);
    puts_(o, key);

    if (plain) {
        puts_(o, "=\"");
        put(o, value, len);
        put(o, "\"", 1);
        return;
    }

    puts_(o, "*=UTF-8''");

    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)value[i];

        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || c == '.' || c == '-' || c == '_') {
            put(o, &value[i], 1);
        } else {
            char e[3] = { '%', hex[c >> 4], hex[c & 15] };

            put(o, e, 3);
        }
    }
}

/* A file's bytes as base64 in lines of 76. */
static void base64_lines(struct out *o, const uint8_t *b, size_t len)
{
    char line[80];

    for (size_t i = 0; i < len; i += 57) {
        size_t n = len - i < 57 ? len - i : 57;
        size_t k = base64_encode(b + i, n, line);

        put(o, line, k);
        crlf(o);
    }
}

/* The boundary between the parts, from the message's id: unique to it. */
static void boundary(char *out, size_t room, const char *id)
{
    uint32_t h = 2166136261u;
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;

    for (const char *p = id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;

    for (const char *p = "=_kosmos_"; *p && n + 1 < room; p++) out[n++] = *p;
    for (int i = 7; i >= 0 && n + 1 < room; i--) out[n++] = hex[(h >> (i * 4)) & 15];
    out[n] = '\0';
}

size_t mail_build(const struct mail_draft *d, uint8_t *out, size_t room)
{
    struct out o = { out, room, 0, 0 };
    int qp = d->natt > 0 || !seven_bit(d->text, d->text_len);
    char bound[40];

    boundary(bound, sizeof bound, d->message_id ? d->message_id : "");

    date_field(&o, d->date, d->zone);

    {
        struct mime_address from;

        memset(&from, 0, sizeof from);
        strncpy(from.name, d->from_name, sizeof from.name - 1);
        strncpy(from.address, d->from_address, sizeof from.address - 1);
        address_field(&o, "From", &from, 1);
    }

    if (d->nto) address_field(&o, "To", d->to, d->nto);
    if (d->ncc) address_field(&o, "Cc", d->cc, d->ncc);

    puts_(&o, "Subject: ");
    {
        size_t len = strlen(d->subject);

        if (ascii(d->subject, len)) plain_words(&o, d->subject, len);
        else encoded_words(&o, d->subject, len);
    }
    crlf(&o);

    puts_(&o, "Message-ID: <");
    puts_(&o, d->message_id);
    puts_(&o, ">");
    crlf(&o);

    if (d->in_reply_to && d->in_reply_to[0]) {
        puts_(&o, "In-Reply-To: <");
        puts_(&o, d->in_reply_to);
        puts_(&o, ">");
        crlf(&o);
    }

    if (d->references && d->references[0]) {
        puts_(&o, "References: ");
        plain_words(&o, d->references, strlen(d->references));
        crlf(&o);
    }

    puts_(&o, "MIME-Version: 1.0");
    crlf(&o);

    if (d->natt > 0) {
        puts_(&o, "Content-Type: multipart/mixed; boundary=\"");
        puts_(&o, bound);
        puts_(&o, "\"");
        crlf(&o);
        crlf(&o);
        puts_(&o, "--");
        puts_(&o, bound);
        crlf(&o);
    }

    puts_(&o, "Content-Type: text/plain; charset=utf-8");
    crlf(&o);
    puts_(&o, qp ? "Content-Transfer-Encoding: quoted-printable" : "Content-Transfer-Encoding: 7bit");
    crlf(&o);
    crlf(&o);

    body_lines(&o, d->text, d->text_len, qp ? qp_line : plain_line);

    for (size_t i = 0; i < d->natt; i++) {
        const struct mail_attachment *a = &d->att[i];
        const char *name = a->name && a->name[0] ? a->name : "attachment";

        puts_(&o, "--");
        puts_(&o, bound);
        crlf(&o);
        puts_(&o, "Content-Type: ");
        puts_(&o, a->type && a->type[0] ? a->type : "application/octet-stream");
        name_param(&o, "name", name);
        crlf(&o);
        puts_(&o, "Content-Disposition: attachment");
        name_param(&o, "filename", name);
        crlf(&o);
        puts_(&o, "Content-Transfer-Encoding: base64");
        crlf(&o);
        crlf(&o);
        base64_lines(&o, a->bytes, a->len);
    }

    if (d->natt > 0) {
        puts_(&o, "--");
        puts_(&o, bound);
        puts_(&o, "--");
        crlf(&o);
    }

    return o.n;
}
