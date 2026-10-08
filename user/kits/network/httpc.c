/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **HTTP from C** (`httpc.h`, `docs/maps.md` M6c).
 *
 * The browser and `fetch` speak HTTP through `http.lua`; nothing in C did,
 * so a server - the map's `tiles` - could not fetch anything. This is the
 * part of HTTP/1.1 a client fetching files needs: a GET written, a reply's
 * head read, its body ended by its length, by chunks or by the close, gzip
 * inflated through the compress kit's one inflater, and the connection kept
 * for the next request unless the reply says otherwise.
 *
 * **Stepped**: `httpc_step` moves what can move and returns; nothing here
 * waits, so a server answers its own callers between steps. The bytes go
 * through `struct httpc_io` - the Network Kit's TCP, the TLS core over it,
 * or a test's replies - and this knows nothing of which.
 */

#include "httpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kits/compress/gzip.h"

void httpc_init(struct httpc *h, struct httpc_io io, const char *host, size_t most)
{
    memset(h, 0, sizeof(*h));
    h->io = io;
    h->body_most = most;
    h->length = -1;
    snprintf(h->host, sizeof(h->host), "%s", host);
}

void httpc_free(struct httpc *h)
{
    free(h->body);
    h->body = NULL;
    h->body_len = h->body_cap = 0;
}

void httpc_reset(struct httpc *h)
{
    h->state = HTTPC_IDLE;
    h->why = NULL;
    h->status = 0;
    h->length = -1;
    h->chunked = h->gzip = 0;
    h->head_len = 0;
    h->body_len = 0;
    h->chunk_left = -1;
    h->line_len = 0;
    h->after_chunk = 0;
}

int httpc_get(struct httpc *h, const char *path)
{
    int n;

    if (h->state != HTTPC_IDLE && h->state != HTTPC_DONE) {
        return -1;
    }

    httpc_reset(h);
    n = snprintf(h->request, sizeof(h->request),
                 "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Kosmos\r\n"
                 "Accept-Encoding: gzip\r\nConnection: keep-alive\r\n\r\n",
                 path, h->host);

    if (n < 0 || (size_t)n >= sizeof(h->request)) {
        h->state = HTTPC_FAILED;
        h->why = "the address is too long";
        return -1;
    }

    h->request_len = (size_t)n;
    h->request_sent = 0;
    h->state = HTTPC_SENDING;
    return 0;
}

int httpc_reusable(const struct httpc *h)
{
    return h->state == HTTPC_DONE && !h->closes;
}

static int fail(struct httpc *h, const char *why)
{
    h->state = HTTPC_FAILED;
    h->why = why;
    return HTTPC_FAILED;
}

/* Bytes of body kept, to `body_most`. */
static int keep(struct httpc *h, const uint8_t *p, size_t n)
{
    if (h->body_len + n > h->body_most) {
        return 0;
    }

    if (h->body_len + n > h->body_cap) {
        size_t cap = h->body_cap ? h->body_cap : 16384;
        uint8_t *grown;

        while (cap < h->body_len + n) cap *= 2;

        if (cap > h->body_most) cap = h->body_most;

        if ((grown = realloc(h->body, cap)) == NULL) {
            return 0;
        }

        h->body = grown;
        h->body_cap = cap;
    }

    memcpy(h->body + h->body_len, p, n);
    h->body_len += n;
    return 1;
}

/* A header's value in the head, case not minded in its name; NULL if not
 * there. `len` its length. */
static const char *header(const struct httpc *h, const char *name, size_t *len)
{
    size_t n = strlen(name);
    const char *p = h->head, *end = h->head + h->head_len;

    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));

        if (eol == NULL) break;

        if ((size_t)(eol - p) > n + 1 && p[n] == ':') {
            size_t i;

            for (i = 0; i < n; i++) {
                char a = p[i], b = name[i];

                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) break;
            }

            if (i == n) {
                const char *v = p + n + 1;

                while (v < eol && (*v == ' ' || *v == '\t')) v++;

                *len = (size_t)(eol - v);
                while (*len > 0 && (v[*len - 1] == '\r' || v[*len - 1] == ' ')) (*len)--;
                return v;
            }
        }

        p = eol + 1;
    }

    return NULL;
}

static int has_word(const char *v, size_t len, const char *word)
{
    size_t n = strlen(word);

    for (size_t i = 0; i + n <= len; i++) {
        size_t k;

        for (k = 0; k < n; k++) {
            char a = v[i + k];

            if (a >= 'A' && a <= 'Z') a += 32;
            if (a != word[k]) break;
        }

        if (k == n) return 1;
    }

    return 0;
}

/* The head read: its status, how the body ends, whether it is gzipped. */
static int parse_head(struct httpc *h)
{
    const char *v;
    size_t len;

    if (h->head_len < 12 || memcmp(h->head, "HTTP/1.", 7) != 0) {
        return fail(h, "the reply is not HTTP");
    }

    h->status = atoi(h->head + 9);

    if ((v = header(h, "content-length", &len)) != NULL) {
        h->length = strtoll(v, NULL, 10);
    }

    if ((v = header(h, "transfer-encoding", &len)) != NULL && has_word(v, len, "chunked")) {
        h->chunked = 1;
        h->chunk_left = -1;
    }

    if ((v = header(h, "content-encoding", &len)) != NULL && has_word(v, len, "gzip")) {
        h->gzip = 1;
    }

    h->closes = (v = header(h, "connection", &len)) != NULL && has_word(v, len, "close");

    /* HTTP/1.0 closes unless it says otherwise. */
    if (h->head[7] == '0' && !((v = header(h, "connection", &len)) != NULL
                               && has_word(v, len, "keep-alive"))) {
        h->closes = 1;
    }

    h->state = HTTPC_BODY;
    return HTTPC_BODY;
}

/* The body inflated in place of itself, when it came gzipped. */
struct gather { uint8_t *bytes; size_t n, cap, most; int over; };

static int gather_put(void *user, const uint8_t *b, size_t n)
{
    struct gather *g = user;

    if (g->n + n > g->most) {
        g->over = 1;
        return 0;
    }

    if (g->n + n > g->cap) {
        size_t cap = g->cap ? g->cap : 65536;
        uint8_t *grown;

        while (cap < g->n + n) cap *= 2;

        if ((grown = realloc(g->bytes, cap)) == NULL) {
            g->over = 1;
            return 0;
        }

        g->bytes = grown, g->cap = cap;
    }

    memcpy(g->bytes + g->n, b, n);
    g->n += n;
    return 1;
}

static int finish(struct httpc *h)
{
    if (h->gzip && h->body_len > 0) {
        struct gather g = { NULL, 0, 0, h->body_most * 8, 0 };
        size_t got;
        struct gunzip_work *work = kosmos_inflater();

        if (work == NULL || kosmos_gunzip(h->body, h->body_len, work, gather_put, &g, &got)
                            != GUNZIP_WHOLE) {
            free(g.bytes);
            return fail(h, "the reply's gzip would not inflate");
        }

        free(h->body);
        h->body = g.bytes;
        h->body_len = g.n;
        h->body_cap = g.cap;
    }

    h->state = HTTPC_DONE;
    return HTTPC_DONE;
}

/* Bytes of a chunked body: sizes, data, the CRLFs between. */
static int chunks(struct httpc *h, const uint8_t *p, size_t n)
{
    size_t i = 0;

    while (i < n) {
        if (h->chunk_left > 0) {
            size_t take = n - i;

            if ((long long)take > h->chunk_left) take = (size_t)h->chunk_left;
            if (!keep(h, p + i, take)) return fail(h, "the reply is larger than allowed");

            h->chunk_left -= (long long)take;
            i += take;

            if (h->chunk_left == 0) {
                h->chunk_left = -1;
                h->after_chunk = 1;
            }

            continue;
        }

        /* A line: a CRLF after data, or a chunk's size in hex. */
        if (p[i] == '\n') {
            h->line[h->line_len] = '\0';

            if (h->after_chunk) {
                h->after_chunk = 0;
            } else if (h->chunk_left == -1) {
                long long size = strtoll(h->line, NULL, 16);

                if (size == 0) {
                    /* The last chunk; trailers are not wanted. */
                    i = n;
                    h->line_len = 0;
                    return finish(h);
                }

                h->chunk_left = size;
            }

            h->line_len = 0;
        } else if (p[i] != '\r' && h->line_len + 1 < sizeof(h->line)) {
            h->line[h->line_len++] = (char)p[i];
        }

        i++;
    }

    return HTTPC_BODY;
}

int httpc_step(struct httpc *h)
{
    uint8_t buf[16384];

    if (h->state == HTTPC_SENDING) {
        long n = h->io.write(h->io.user, h->request + h->request_sent,
                             h->request_len - h->request_sent);

        if (n < 0) return fail(h, "the connection closed before the request went");

        h->request_sent += (size_t)n;

        if (h->request_sent < h->request_len) return HTTPC_SENDING;

        h->state = HTTPC_HEAD;
    }

    while (h->state == HTTPC_HEAD || h->state == HTTPC_BODY) {
        long n;

        if (h->state == HTTPC_HEAD) {
            /* A byte at a time into the head until its blank line: what
             * follows is the body's, and stays for it. */
            n = h->io.read(h->io.user, buf, 1);

            if (n < 0) return fail(h, "the connection closed before the reply");
            if (n == 0) return HTTPC_HEAD;

            if (h->head_len + 1 >= sizeof(h->head)) return fail(h, "the reply's head is too long");

            h->head[h->head_len++] = (char)buf[0];

            if (h->head_len >= 4 && memcmp(h->head + h->head_len - 4, "\r\n\r\n", 4) == 0) {
                if (parse_head(h) == HTTPC_FAILED) return HTTPC_FAILED;

                if (!h->chunked && h->length == 0) return finish(h);
            }

            continue;
        }

        {
            size_t want = sizeof(buf);

            if (!h->chunked && h->length >= 0) {
                long long left = h->length - (long long)h->body_len;

                if (left <= 0) return finish(h);
                if ((long long)want > left) want = (size_t)left;
            }

            n = h->io.read(h->io.user, buf, want);
        }

        if (n == 0) return HTTPC_BODY;

        if (n < 0) {
            /* Over: the end of a body that ends with its connection. */
            if (!h->chunked && h->length < 0) {
                h->closes = 1;
                return finish(h);
            }

            return fail(h, "the connection closed in the middle of the reply");
        }

        if (h->chunked) {
            int r = chunks(h, buf, (size_t)n);

            if (r != HTTPC_BODY) return r;
        } else {
            if (!keep(h, buf, (size_t)n)) return fail(h, "the reply is larger than allowed");

            if (h->length >= 0 && (long long)h->body_len >= h->length) return finish(h);
        }
    }

    return h->state;
}
