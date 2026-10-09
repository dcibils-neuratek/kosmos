/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A message read (`mime.h`, `docs/mail.md` M1): its header fields found and
 * unfolded, encoded words decoded, its parts found by their boundaries, and
 * a part's content undone - base64 and quoted-printable, then its
 * characters to UTF-8 (`charset.c`).
 *
 * **Nothing here trusts the message.** A field, a boundary or a length that
 * runs past the bytes stops where the bytes stop; a multipart nested past
 * `MIME_DEPTH_MOST`, or past `MIME_PARTS_MOST` parts, is read no deeper;
 * every copy is bounded by the room it is given. A message is something a
 * stranger sends.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mime.h"
#include "kits/compress/base64_core.h"

/* ------------------------------------------------------------------------
 * Small things.
 * --------------------------------------------------------------------- */

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int same_ci(const char *a, size_t an, const char *b)
{
    size_t bn = strlen(b);

    if (an != bn) return 0;

    for (size_t i = 0; i < an; i++) {
        if (lc((unsigned char)a[i]) != lc((unsigned char)b[i])) return 0;
    }

    return 1;
}

static void copy_lower(char *out, size_t room, const char *in, size_t n)
{
    size_t i = 0;

    for (; i < n && i + 1 < room; i++) out[i] = (char)lc((unsigned char)in[i]);
    out[i] = '\0';
}

static void copy_text(char *out, size_t room, const char *in, size_t n)
{
    if (room == 0) return;
    if (n >= room) n = room - 1;

    memcpy(out, in, n);
    out[n] = '\0';
}

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Where a line that starts at `at` ends, past its line end; `len` at most. */
static size_t next_line(const uint8_t *b, size_t at, size_t len)
{
    while (at < len && b[at] != '\n') at++;
    return at < len ? at + 1 : len;
}

/* ------------------------------------------------------------------------
 * The header block: where it ends, and a field in it.
 * --------------------------------------------------------------------- */

/* The length of the header block at `at` - up to, not including, the blank
 * line - and where the body starts. A block with no blank line is all
 * header and no body. */
static void split_head(const uint8_t *b, size_t at, size_t end, size_t *head_len, size_t *body)
{
    size_t p = at;

    while (p < end) {
        if (b[p] == '\n') {
            *head_len = p - at;
            *body = p + 1;
            return;
        }

        if (b[p] == '\r' && p + 1 < end && b[p + 1] == '\n') {
            *head_len = p - at;
            *body = p + 2;
            return;
        }

        p = next_line(b, p, end);
    }

    *head_len = end - at;
    *body = end;
}

/* Field `name`'s raw value in the block - folded, as it is - its start and
 * length; 0 when there is none. The first of several. */
static int raw_field(const uint8_t *b, size_t head, size_t head_len, const char *name,
                     size_t *val, size_t *val_len)
{
    size_t end = head + head_len, p = head;
    size_t nlen = strlen(name);

    while (p < end) {
        size_t line_end = next_line(b, p, end);

        if (!is_space(b[p])) {
            size_t colon = p;

            while (colon < line_end && b[colon] != ':') colon++;

            if (colon < line_end && same_ci((const char *)b + p, colon - p, name) && colon - p == nlen) {
                size_t v = colon + 1, e = line_end;

                /* And the lines folded under it. */
                while (e < end && (b[e] == ' ' || b[e] == '\t')) e = next_line(b, e, end);

                *val = v;
                *val_len = e - v;
                return 1;
            }
        }

        p = line_end;
    }

    return 0;
}

/* A value unfolded: its line ends dropped, its ends trimmed. */
static size_t unfold(const uint8_t *b, size_t at, size_t n, char *out, size_t room)
{
    size_t o = 0;

    for (size_t i = 0; i < n && o + 1 < room; i++) {
        uint8_t c = b[at + i];

        /* A fold - a line end and the space after it - is one space, as
         * every mail client shows it. */
        if (c == '\r' || c == '\n') {
            while (i + 1 < n && (b[at + i + 1] == '\r' || b[at + i + 1] == '\n'
                                 || b[at + i + 1] == ' ' || b[at + i + 1] == '\t')) {
                i++;
            }

            if (o > 0 && out[o - 1] != ' ') out[o++] = ' ';
            continue;
        }

        out[o++] = (char)c;
    }

    while (o > 0 && is_space((unsigned char)out[o - 1])) o--;

    size_t s = 0;

    while (s < o && is_space((unsigned char)out[s])) s++;

    memmove(out, out + s, o - s);
    o -= s;
    out[o] = '\0';
    return o;
}

/* ------------------------------------------------------------------------
 * Encoded words (RFC 2047): =?charset?B?...?= and =?charset?Q?...?=.
 * --------------------------------------------------------------------- */

/* An encoded word at `s` decoded into `out` as UTF-8; its length in `s`, or
 * 0 when it is not one. */
static size_t encoded_word(const char *s, size_t n, uint8_t *out, size_t room, size_t *made)
{
    char charset[40], enc;
    size_t i = 2, cs, text, text_end;
    uint8_t raw[1024];
    size_t r = 0;

    *made = 0;

    if (n < 8 || s[0] != '=' || s[1] != '?') return 0;

    cs = i;
    while (i < n && s[i] != '?') i++;
    if (i >= n || i - cs == 0) return 0;

    copy_lower(charset, sizeof charset, s + cs, i - cs);

    /* A language after a star, RFC 2231's: dropped. */
    for (char *star = charset; *star; star++) if (*star == '*') { *star = '\0'; break; }

    i++;
    if (i + 1 >= n || s[i + 1] != '?') return 0;

    enc = (char)lc((unsigned char)s[i]);
    if (enc != 'b' && enc != 'q') return 0;

    text = i + 2;
    text_end = text;
    while (text_end + 1 < n && !(s[text_end] == '?' && s[text_end + 1] == '=')) text_end++;
    if (text_end + 1 >= n) return 0;

    if (enc == 'b') {
        size_t bad;

        r = base64_decode(s + text, text_end - text, raw, sizeof raw, 1, &bad);
    } else {
        for (size_t k = text; k < text_end && r < sizeof raw; k++) {
            if (s[k] == '_') {
                raw[r++] = ' ';
            } else if (s[k] == '=' && k + 2 < text_end + 1 && hexval(s[k + 1]) >= 0
                       && hexval(s[k + 2]) >= 0) {
                raw[r++] = (uint8_t)(hexval(s[k + 1]) << 4 | hexval(s[k + 2]));
                k += 2;
            } else {
                raw[r++] = (uint8_t)s[k];
            }
        }
    }

    *made = mime_to_utf8(charset, raw, r, out, room);
    return text_end + 2;
}

/* A field's value with its encoded words decoded; space between two encoded
 * words dropped, as the RFC has it. Bytes outside them taken as UTF-8. */
static size_t decode_words(const char *in, size_t n, char *out, size_t room)
{
    size_t o = 0, i = 0;
    int after_word = 0;

    if (room == 0) return 0;

    while (i < n && o + 1 < room) {
        size_t made, used;

        if (in[i] == '=' && i + 1 < n && in[i + 1] == '?'
            && (used = encoded_word(in + i, n - i, (uint8_t *)out + o, room - 1 - o, &made)) > 0) {
            o += made;
            i += used;
            after_word = 1;
            continue;
        }

        /* Space between two encoded words is not part of either. */
        if (after_word && is_space((unsigned char)in[i])) {
            size_t k = i;

            while (k < n && is_space((unsigned char)in[k])) k++;

            if (k + 1 < n && in[k] == '=' && in[k + 1] == '?') {
                i = k;
                continue;
            }
        }

        after_word = 0;

        /* Plain bytes, kept well-formed. */
        {
            size_t run = i;

            while (run < n && !(in[run] == '=' && run + 1 < n && in[run + 1] == '?')) run++;
            if (run == i) run = i + 1;

            o += mime_to_utf8("utf-8", (const uint8_t *)in + i, run - i, (uint8_t *)out + o,
                              room - 1 - o);
            i = run;
        }
    }

    out[o] = '\0';
    return o;
}

long mime_header(const struct mime_msg *m, size_t p, const char *name, char *out, size_t room)
{
    char raw[4096];
    size_t v, vn, n;
    const struct mime_part *part;

    if (p >= m->nparts || room == 0) return -1;

    part = &m->part[p];

    if (!raw_field(m->buf, part->head, part->head_len, name, &v, &vn)) return -1;

    n = unfold(m->buf, v, vn, raw, sizeof raw);
    return (long)decode_words(raw, n, out, room);
}

/* ------------------------------------------------------------------------
 * A field's value and its parameters: "text/plain; charset=utf-8".
 * --------------------------------------------------------------------- */

/* The value before the first ';', lower case. */
static void main_value(const char *v, char *out, size_t room)
{
    size_t n = 0;

    while (v[n] && v[n] != ';') n++;
    while (n > 0 && is_space((unsigned char)v[n - 1])) n--;

    size_t s = 0;

    while (s < n && is_space((unsigned char)v[s])) s++;

    copy_lower(out, room, v + s, n - s);
}

/* Parameter `want`'s value, quotes and backslashes undone; RFC 2231's
 * `name*=charset''pct-encoded` and `name*0`, `name*1`... continuations put
 * together and made UTF-8. 0 when there is none. */
static int param(const char *v, const char *want, char *out, size_t room)
{
    char joined[1024], charset[40] = "";
    size_t jo = 0, wl = strlen(want);
    int found = 0, extended = 0;

    out[0] = '\0';

    for (const char *p = strchr(v, ';'); p; p = strchr(p, ';')) {
        const char *k, *ke, *val;
        char vbuf[1024];
        size_t vo = 0;

        p++;
        while (is_space((unsigned char)*p)) p++;

        k = p;
        while (*p && *p != '=' && *p != ';') p++;
        if (*p != '=') continue;

        ke = p;
        while (ke > k && is_space((unsigned char)ke[-1])) ke--;

        val = p + 1;
        while (is_space((unsigned char)*val)) val++;

        if (*val == '"') {
            val++;
            while (*val && *val != '"' && vo + 1 < sizeof vbuf) {
                if (*val == '\\' && val[1]) val++;
                vbuf[vo++] = *val++;
            }
        } else {
            while (*val && *val != ';' && !is_space((unsigned char)*val) && vo + 1 < sizeof vbuf) {
                vbuf[vo++] = *val++;
            }
        }

        vbuf[vo] = '\0';
        p = val;

        /* `want`, `want*`, `want*0`, `want*0*`, `want*1`... */
        size_t kl = (size_t)(ke - k);

        if (kl < wl || !same_ci(k, wl, want)) continue;

        const char *rest = k + wl;
        size_t restn = kl - wl;
        int star_last = restn > 0 && rest[restn - 1] == '*';

        if (restn == 0) {
            if (!found) {
                copy_text(joined, sizeof joined, vbuf, vo);
                jo = strlen(joined);
            }
            found = 1;
            continue;
        }

        if (rest[0] != '*') continue;

        /* An extended value: the first piece names its charset. */
        const char *text = vbuf;

        if (star_last) {
            extended = 1;

            const char *q1 = strchr(vbuf, '\'');
            const char *q2 = q1 ? strchr(q1 + 1, '\'') : NULL;

            if (q1 && q2) {
                if (charset[0] == '\0') copy_lower(charset, sizeof charset, vbuf, (size_t)(q1 - vbuf));
                text = q2 + 1;
            }
        }

        if (!found || extended) {
            if (!found) jo = 0;

            for (const char *t = text; *t && jo + 1 < sizeof joined; t++) {
                if (star_last && *t == '%' && hexval(t[1]) >= 0 && hexval(t[2]) >= 0) {
                    joined[jo++] = (char)(hexval(t[1]) << 4 | hexval(t[2]));
                    t += 2;
                } else {
                    joined[jo++] = *t;
                }
            }

            found = 1;
        }
    }

    if (!found) return 0;

    joined[jo] = '\0';

    if (extended) {
        size_t n = mime_to_utf8(charset[0] ? charset : "utf-8", (const uint8_t *)joined, jo,
                                (uint8_t *)out, room - 1);
        out[n] = '\0';
    } else {
        /* An encoded word in a quoted name, which mail clients write though
         * RFC 2047 says not to. */
        decode_words(joined, jo, out, room);
    }

    return 1;
}

/* ------------------------------------------------------------------------
 * Parts.
 * --------------------------------------------------------------------- */

/* One part's facts, from its header block; `parent_type` for the default
 * a multipart/digest gives its parts. */
static void read_facts(struct mime_msg *m, struct mime_part *part)
{
    char v[2048];
    size_t at, n;

    strcpy(part->type, "text/plain");
    part->charset[0] = part->encoding[0] = part->name[0] = '\0';
    part->disposition[0] = part->cid[0] = '\0';

    if (raw_field(m->buf, part->head, part->head_len, "content-type", &at, &n)) {
        unfold(m->buf, at, n, v, sizeof v);
        main_value(v, part->type, sizeof part->type);

        if (strchr(part->type, '/') == NULL) strcpy(part->type, "text/plain");

        char cs[80];

        if (param(v, "charset", cs, sizeof cs)) copy_lower(part->charset, sizeof part->charset, cs, strlen(cs));

        param(v, "name", part->name, sizeof part->name);
    }

    if (raw_field(m->buf, part->head, part->head_len, "content-transfer-encoding", &at, &n)) {
        unfold(m->buf, at, n, v, sizeof v);
        main_value(v, part->encoding, sizeof part->encoding);
    }

    if (raw_field(m->buf, part->head, part->head_len, "content-disposition", &at, &n)) {
        char fname[160];

        unfold(m->buf, at, n, v, sizeof v);
        main_value(v, part->disposition, sizeof part->disposition);

        if (param(v, "filename", fname, sizeof fname)) copy_text(part->name, sizeof part->name, fname, strlen(fname));
    }

    if (raw_field(m->buf, part->head, part->head_len, "content-id", &at, &n)) {
        size_t k = unfold(m->buf, at, n, v, sizeof v), s = 0;

        if (k > 0 && v[0] == '<') s = 1;
        if (k > s && v[k - 1] == '>') k--;

        copy_text(part->cid, sizeof part->cid, v + s, k - s);
    }

    part->multipart = strncmp(part->type, "multipart/", 10) == 0;
}

/* Whether a delimiter line, "--boundary" or "--boundary--", starts at `at`;
 * 2 for the closing one, 1 for another, 0 for neither. */
static int delimiter(const uint8_t *b, size_t at, size_t end, const char *boundary, size_t bl)
{
    size_t k = at + 2 + bl;
    int closing = 0;

    if (k > end || b[at] != '-' || b[at + 1] != '-'
        || memcmp(b + at + 2, boundary, bl) != 0) {
        return 0;
    }

    if (k + 1 < end && b[k] == '-' && b[k + 1] == '-') {
        closing = 1;
        k += 2;
    }

    /* Nothing but space to the line's end (RFC 2046 5.1.1): "--d1" is not
     * the line "--d10". */
    while (k < end && (b[k] == ' ' || b[k] == '\t')) k++;

    if (k < end && b[k] != '\r' && b[k] != '\n') return 0;

    return closing ? 2 : 1;
}

static void add_part(struct mime_msg *m, int parent, int depth, size_t at, size_t end);

/* The children of multipart `p`: what lies between its delimiter lines. */
static void children(struct mime_msg *m, size_t p)
{
    struct mime_part *part = &m->part[p];
    char v[2048], boundary[200];
    size_t at, n, bl;

    if (part->depth + 1 >= (int)MIME_DEPTH_MOST
        || !raw_field(m->buf, part->head, part->head_len, "content-type", &at, &n)) {
        part->multipart = 0;
        return;
    }

    unfold(m->buf, at, n, v, sizeof v);

    if (!param(v, "boundary", boundary, sizeof boundary) || (bl = strlen(boundary)) == 0) {
        part->multipart = 0;
        return;
    }

    size_t end = part->body + part->body_len, line = part->body, start = 0;
    int open = 0;

    while (line < end) {
        int d = delimiter(m->buf, line, end, boundary, bl);
        size_t next = next_line(m->buf, line, end);

        if (d) {
            if (open) {
                /* The line end before the delimiter is the delimiter's. */
                size_t stop = line;

                if (stop > start && m->buf[stop - 1] == '\n') stop--;
                if (stop > start && m->buf[stop - 1] == '\r') stop--;

                add_part(m, (int)p, part->depth + 1, start, stop);
            }

            if (d == 2) return;

            open = 1;
            start = next;
        }

        line = next;
    }

    /* Cut short, no closing delimiter: what was open is a part. */
    if (open && start < end) add_part(m, (int)p, part->depth + 1, start, end);
}

static void add_part(struct mime_msg *m, int parent, int depth, size_t at, size_t end)
{
    struct mime_part *part;
    size_t p;

    if (m->nparts >= MIME_PARTS_MOST) return;

    p = m->nparts++;
    part = &m->part[p];
    memset(part, 0, sizeof *part);
    part->parent = parent;
    part->depth = depth;
    part->head = at;
    split_head(m->buf, at, end, &part->head_len, &part->body);
    part->body_len = end - part->body;

    read_facts(m, part);

    if (part->multipart) children(m, p);
}

int mime_parse(struct mime_msg *m, const uint8_t *buf, size_t len)
{
    m->buf = buf;
    m->len = len;
    m->nparts = 0;

    if (len == 0) return -1;

    add_part(m, -1, 0, 0, len);

    return m->part[0].head_len > 0 ? 0 : -1;
}

/* ------------------------------------------------------------------------
 * A part's content.
 * --------------------------------------------------------------------- */

size_t mime_part_bound(const struct mime_msg *m, size_t p)
{
    if (p >= m->nparts) return 0;

    /* A single byte can become three of UTF-8; nothing else grows. */
    return m->part[p].body_len * 3 + 4;
}

/* Quoted-printable undone: "=XX", and "=" at a line's end joining it to the
 * next. Into `out`; how many. */
static size_t unquote(const uint8_t *in, size_t n, uint8_t *out, size_t room)
{
    size_t o = 0;

    for (size_t i = 0; i < n && o < room; i++) {
        if (in[i] != '=') {
            out[o++] = in[i];
            continue;
        }

        /* A soft line break: "=" and spaces to the line's end. */
        size_t k = i + 1;

        while (k < n && (in[k] == ' ' || in[k] == '\t')) k++;

        if (k < n && (in[k] == '\r' || in[k] == '\n')) {
            if (in[k] == '\r' && k + 1 < n && in[k + 1] == '\n') k++;
            i = k;
            continue;
        }

        if (k >= n) break;

        if (i + 2 < n && hexval(in[i + 1]) >= 0 && hexval(in[i + 2]) >= 0) {
            out[o++] = (uint8_t)(hexval(in[i + 1]) << 4 | hexval(in[i + 2]));
            i += 2;
        } else {
            out[o++] = '=';
        }
    }

    return o;
}

size_t mime_part_bytes(const struct mime_msg *m, size_t p, uint8_t *out, size_t room)
{
    const struct mime_part *part;
    const uint8_t *body;
    size_t n, got;

    if (p >= m->nparts) return 0;

    part = &m->part[p];
    body = m->buf + part->body;
    n = part->body_len;

    /*
     * A text part is undone into the far end of `out` and made UTF-8 from
     * there into its start. With `room` at least `mime_part_bound` - three
     * bytes for each - the writing never overtakes the reading: after `i`
     * bytes read it has written at most `3 * i`, and the reading is at
     * `2 * n + 4 + i`. With less room a text part is refused, rather than
     * half made.
     */
    int text = strncmp(part->type, "text/", 5) == 0;

    if (text && room < mime_part_bound(m, p)) return 0;

    uint8_t *undone = text ? out + (room - n) : out;
    size_t undone_room = text ? n : room;

    if (strcmp(part->encoding, "base64") == 0) {
        size_t bad;

        got = base64_decode((const char *)body, n, undone, undone_room, 1, &bad);
    } else if (strcmp(part->encoding, "quoted-printable") == 0) {
        got = unquote(body, n, undone, undone_room);
    } else {
        got = n < undone_room ? n : undone_room;
        memmove(undone, body, got);
    }

    if (!text) return got;

    return mime_to_utf8(part->charset, undone, got, out, room);
}

/* ------------------------------------------------------------------------
 * Dates (RFC 5322 3.3): "Tue, 6 Oct 2026 09:41:07 +0200".
 * --------------------------------------------------------------------- */

/* Days from 1970-01-01 to a civil date (Howard Hinnant's). */
static int64_t days_from_civil(int64_t y, unsigned mo, unsigned d)
{
    y -= mo <= 2;

    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + (int64_t)doe - 719468;
}

static const char *number(const char *p, long *v, int *digits)
{
    *v = 0, *digits = 0;

    while (*p >= '0' && *p <= '9') {
        *v = *v * 10 + (*p - '0');
        (*digits)++;
        p++;
    }

    return p;
}

int64_t mime_date(const char *value, int *ok)
{
    static const char *months[] = { "jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec" };
    const char *p = value;
    long day, year, hh, mm, ss = 0;
    int dg, month = -1;

    *ok = 0;

    while (is_space((unsigned char)*p)) p++;

    /* A day of the week, which says nothing the date does not. */
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }

    while (is_space((unsigned char)*p)) p++;
    p = number(p, &day, &dg);
    if (dg == 0) return 0;

    while (is_space((unsigned char)*p)) p++;

    for (int i = 0; i < 12; i++) {
        if (lc((unsigned char)p[0]) == months[i][0] && lc((unsigned char)p[1]) == months[i][1]
            && lc((unsigned char)p[2]) == months[i][2]) {
            month = i + 1;
        }
    }

    if (month < 0) return 0;

    while (*p && !is_space((unsigned char)*p)) p++;
    while (is_space((unsigned char)*p)) p++;

    p = number(p, &year, &dg);
    if (dg == 0) return 0;
    if (dg <= 2) year += year < 50 ? 2000 : 1900;     /* RFC 5322 4.3 */

    while (is_space((unsigned char)*p)) p++;
    p = number(p, &hh, &dg);
    if (dg == 0 || *p != ':') return 0;
    p = number(p + 1, &mm, &dg);
    if (dg == 0) return 0;
    if (*p == ':') {
        p = number(p + 1, &ss, &dg);
        if (dg == 0) return 0;
    }

    while (is_space((unsigned char)*p)) p++;

    /* The zone: +hhmm, -hhmm, or one of the old names. */
    long zone = 0;

    if (*p == '+' || *p == '-') {
        long z;
        int sign = *p == '-' ? -1 : 1;

        number(p + 1, &z, &dg);
        if (dg == 4) zone = sign * ((z / 100) * 3600 + (z % 100) * 60);
    } else {
        static const struct { const char *n; int h; } names[] = {
            { "UT", 0 }, { "GMT", 0 }, { "Z", 0 }, { "EST", -5 }, { "EDT", -4 },
            { "CST", -6 }, { "CDT", -5 }, { "MST", -7 }, { "MDT", -6 }, { "PST", -8 }, { "PDT", -7 },
        };

        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            size_t k = strlen(names[i].n);

            if (strncmp(p, names[i].n, k) == 0 && !((p[k] >= 'A' && p[k] <= 'Z'))) {
                zone = names[i].h * 3600L;
            }
        }
    }

    if (day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60) return 0;

    *ok = 1;
    return days_from_civil(year, (unsigned)month, (unsigned)day) * 86400
           + hh * 3600 + mm * 60 + ss - zone;
}

/* ------------------------------------------------------------------------
 * Addresses: "Lena Moreau <lena@example.com>, \"Ferreira, T.\" <t@x.org>".
 * --------------------------------------------------------------------- */

static void trim_copy(char *out, size_t room, const char *s, size_t n)
{
    while (n > 0 && is_space((unsigned char)*s)) s++, n--;
    while (n > 0 && is_space((unsigned char)s[n - 1])) n--;

    /* A display name in quotes: without them, its escapes undone. */
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        size_t o = 0;

        for (size_t i = 1; i + 1 < n && o + 1 < room; i++) {
            if (s[i] == '\\' && i + 2 < n) i++;
            out[o++] = s[i];
        }

        out[o] = '\0';
        return;
    }

    copy_text(out, room, s, n);
}

size_t mime_addresses(const char *value, struct mime_address *out, size_t most)
{
    size_t count = 0;
    const char *p = value;

    while (*p && count < most) {
        const char *start = p, *lt = NULL, *gt = NULL;
        int quoted = 0, paren = 0;

        /* To the comma that ends this one - not one in quotes or < >. */
        for (; *p; p++) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') quoted = !quoted;
            else if (!quoted && *p == '(') paren++;
            else if (!quoted && *p == ')' && paren > 0) paren--;
            else if (!quoted && !paren && *p == '<') lt = p;
            else if (!quoted && !paren && *p == '>' && lt) gt = p;
            else if (!quoted && !paren && (*p == ',' || *p == ';') && (!lt || gt)) break;
        }

        struct mime_address *a = &out[count];

        a->name[0] = a->address[0] = '\0';

        if (lt && gt && gt > lt) {
            trim_copy(a->address, sizeof a->address, lt + 1, (size_t)(gt - lt - 1));
            trim_copy(a->name, sizeof a->name, start, (size_t)(lt - start));
        } else {
            /* A bare address, perhaps with a (comment) after it. */
            const char *e = start;

            while (e < p && *e != '(') e++;
            trim_copy(a->address, sizeof a->address, start, (size_t)(e - start));

            if (e < p) {
                const char *c = e + 1, *ce = c;

                while (ce < p && *ce != ')') ce++;
                trim_copy(a->name, sizeof a->name, c, (size_t)(ce - c));
            }

            /* A group's name, "Friends: a@x, b@y;", is not an address. */
            char *colon = strchr(a->address, ':');

            if (colon) memmove(a->address, colon + 1, strlen(colon + 1) + 1);
            trim_copy(a->address, sizeof a->address, a->address, strlen(a->address));
        }

        if (strchr(a->address, '@') != NULL) count++;

        if (*p) p++;
    }

    return count;
}

/* ------------------------------------------------------------------------
 * The first words, for the list.
 * --------------------------------------------------------------------- */

/* The part a reader would read first: plain text not attached, else HTML. */
static long reading_part(const struct mime_msg *m, const char *type)
{
    for (size_t i = 0; i < m->nparts; i++) {
        const struct mime_part *part = &m->part[i];

        if (!part->multipart && strcmp(part->type, type) == 0
            && strcmp(part->disposition, "attachment") != 0) {
            return (long)i;
        }
    }

    return -1;
}

/* Whether `p` starts with `word`, in any case. */
static int lower_is(const uint8_t *p, const char *word, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = p[i];

        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        if (c != (uint8_t)word[i]) return 0;
    }

    return 1;
}

size_t mime_preview(const struct mime_msg *m, char *out, size_t room)
{
    static uint8_t buf[256 * 1024];
    long p = reading_part(m, "text/plain");
    int html = 0;
    size_t n, o = 0;

    if (room == 0) return 0;

    if (p < 0) {
        p = reading_part(m, "text/html");
        html = 1;
    }

    out[0] = '\0';

    if (p < 0) return 0;

    /* A preview needs the start of a part, not all of it. */
    {
        struct mime_msg *mm = (struct mime_msg *)m;
        size_t keep = mm->part[p].body_len;
        size_t most = (sizeof buf - 4) / 3;

        if (keep > most) mm->part[p].body_len = most;
        n = mime_part_bytes(m, (size_t)p, buf, sizeof buf);
        mm->part[p].body_len = keep;
    }

    int in_tag = 0, at_line = 1, quote = 0, space = 0;

    for (size_t i = 0; i < n && o + 1 < room; i++) {
        uint8_t c = buf[i];

        if (html) {
            if (c == '<') {
                /* What a head, a style sheet or a script holds is not words
                 * anybody wrote: the whole of it is passed over, to its
                 * closing tag. */
                static const char *const skip[] = { "head", "style", "script", "title" };
                size_t after = i;

                for (size_t k = 0; k < sizeof skip / sizeof skip[0] && after == i; k++) {
                    size_t l = strlen(skip[k]);

                    if (i + 1 + l < n && lower_is(buf + i + 1, skip[k], l)
                        && (buf[i + 1 + l] == '>' || buf[i + 1 + l] == ' '
                            || buf[i + 1 + l] == '\t' || buf[i + 1 + l] == '\n'
                            || buf[i + 1 + l] == '\r')) {
                        for (size_t j = i + 1 + l; j + 2 + l < n; j++) {
                            if (buf[j] == '<' && buf[j + 1] == '/'
                                && lower_is(buf + j + 2, skip[k], l)) {
                                after = j + 2 + l;
                                break;
                            }
                        }

                        if (after == i) after = n;      /* never closed: the rest */
                    }
                }

                if (after != i) i = after;              /* on its close's name */
                in_tag = 1;
                space = 1;
                continue;
            }
            if (in_tag) { if (c == '>') in_tag = 0; continue; }
            if (c == '&') {
                static const struct { const char *e; char c; } ents[] = {
                    { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' },
                    { "&#39;", '\'' }, { "&nbsp;", ' ' },
                };
                int done = 0;

                for (size_t e = 0; e < sizeof ents / sizeof ents[0] && !done; e++) {
                    size_t k = strlen(ents[e].e);

                    if (i + k <= n && memcmp(buf + i, ents[e].e, k) == 0) {
                        c = (uint8_t)ents[e].c;
                        i += k - 1;
                        done = 1;
                    }
                }
            }
        }

        if (c == '\n') {
            at_line = 1, quote = 0, space = 1;
            continue;
        }

        if (at_line && c == '>' && !html) quote = 1;
        if (!is_space(c)) at_line = 0;
        if (quote) continue;

        if (is_space(c)) {
            space = 1;
            continue;
        }

        if (space && o > 0 && o + 1 < room) out[o++] = ' ';
        space = 0;
        out[o++] = (char)c;
    }

    /* Not a character cut in two. */
    while (o > 0 && ((uint8_t)out[o - 1] & 0xC0) == 0x80) o--;
    if (o > 0 && ((uint8_t)out[o - 1] & 0x80)) o--;

    out[o] = '\0';
    return o;
}
