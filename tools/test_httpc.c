/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * HTTP from C, on the Mac (`user/kits/network/httpc.c`, `docs/maps.md`
 * M6c): the client held to replies written here, each handed over in
 * pieces of every size from one byte to all of it - a body by its length,
 * two requests on one connection, chunks, gzip, a body ended by the close,
 * a 404, a reply larger than allowed and one cut short.
 */

#include "httpc.h"
#include "kits/compress/gzip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

/* The compress kit's inflater, which in the image lives in pages of its own. */
static struct gunzip_work work;
struct gunzip_work *kosmos_inflater(void) { return &work; }

/* A fake connection: what the client wrote, and a reply handed out
 * `piece` bytes a read, then over (-1) or nothing more yet (0). */
struct fake {
    char        wrote[4096];
    size_t      wrote_len;
    const char *reply;
    size_t      reply_len, at, piece;
    int         over;
};

static long fake_write(void *user, const void *p, size_t n)
{
    struct fake *f = user;

    if (f->wrote_len + n > sizeof(f->wrote)) n = sizeof(f->wrote) - f->wrote_len;

    memcpy(f->wrote + f->wrote_len, p, n);
    f->wrote_len += n;
    return (long)n;
}

static long fake_read(void *user, void *buf, size_t max)
{
    struct fake *f = user;
    size_t n = f->reply_len - f->at;

    if (n == 0) return f->over ? -1 : 0;
    if (n > f->piece) n = f->piece;
    if (n > max) n = max;

    memcpy(buf, f->reply + f->at, n);
    f->at += n;
    return (long)n;
}

/* Steps until done or failed, or until it stops moving. */
static int run(struct httpc *h)
{
    int r = HTTPC_SENDING;

    for (int i = 0; i < 100000; i++) {
        r = httpc_step(h);

        if (r == HTTPC_DONE || r == HTTPC_FAILED) return r;
    }

    return r;
}

/* A sink of a few bytes' room, emptied between steps into `all`: a
 * download's region, written out and taken, in small. */
struct pail {
    char   room_for[7];
    size_t in, cap;
    char   all[512];
    size_t all_len;
    int    full_seen;
};

static size_t pail_room(void *user)
{
    struct pail *p = user;
    size_t room = p->cap - p->in;

    if (room == 0) p->full_seen = 1;
    return room;
}

static int pail_put(void *user, const uint8_t *b, size_t n)
{
    struct pail *p = user;

    if (p->in + n > p->cap) return 0;

    memcpy(p->room_for + p->in, b, n);
    p->in += n;
    return 1;
}

/* Steps, the pail emptied after each, until done or failed. */
static int run_into(struct httpc *h, struct pail *p)
{
    int r = HTTPC_SENDING;

    for (int i = 0; i < 100000; i++) {
        r = httpc_step(h);

        memcpy(p->all + p->all_len, p->room_for, p->in);
        p->all_len += p->in;
        p->in = 0;

        if (r == HTTPC_DONE || r == HTTPC_FAILED) return r;
    }

    return r;
}

static const char GZ[] = "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\x4b\x54\x28\x4b\x4d"
                         "\x2e\xc9\x2f\x52\x28\xc9\xcc\x49\xd5\x51\xc8\xcc\x4b\xcb\x49"
                         "\x2c\x49\x4d\x01\x00\x92\x83\xf8\x66\x17\x00\x00\x00";

int main(void)
{
    char said[300];

    for (size_t piece = 1; piece <= 4096; piece = piece < 8 ? piece + 1 : piece * 4) {
        struct fake f;
        struct httpc h;
        struct httpc_io io = { &f, fake_write, fake_read };
        char reply[512];
        int r;

        /* 1. A body by its length; the request as it should be. */
        memset(&f, 0, sizeof(f));
        f.piece = piece;
        f.reply = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 11\r\n\r\nhello world";
        f.reply_len = strlen(f.reply);
        httpc_init(&h, io, "tiles.example", 1 << 20);
        httpc_get(&h, "/planet/14/1/2.pbf");
        r = run(&h);
        snprintf(said, sizeof said, "in pieces of %zu, a body by its length: %d %d %.*s", piece, r,
                 h.status, (int)h.body_len, h.body ? (char *)h.body : "");
        check(r == HTTPC_DONE && h.status == 200 && h.body_len == 11
              && memcmp(h.body, "hello world", 11) == 0 && httpc_reusable(&h), said);
        check(strstr(f.wrote, "GET /planet/14/1/2.pbf HTTP/1.1\r\n") == f.wrote
              && strstr(f.wrote, "Host: tiles.example\r\n") != NULL
              && strstr(f.wrote, "Accept-Encoding: gzip\r\n") != NULL
              && strstr(f.wrote, "User-Agent: Kosmos/1 (+https://") != NULL
              && f.wrote_len > 4 && memcmp(f.wrote + f.wrote_len - 4, "\r\n\r\n", 4) == 0,
              "the request was not a GET with its Host, User-Agent and Accept-Encoding");

        /* 2. The same connection again: chunks, with an extension. */
        f.reply = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                  "4;x=y\r\nWiki\r\n5\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\n\r\n";
        f.reply_len = strlen(f.reply);
        f.at = 0;
        f.wrote_len = 0;
        check(httpc_get(&h, "/b") == 0, "a second request on a kept connection was refused");
        r = run(&h);
        snprintf(said, sizeof said, "in pieces of %zu, chunks came to %d %.*s", piece, r,
                 (int)h.body_len, h.body ? (char *)h.body : "");
        check(r == HTTPC_DONE && h.body_len == 23
              && memcmp(h.body, "Wikipedia in\r\n\r\nchunks.", 23) == 0, said);
        httpc_free(&h);

        /* 3. Gzipped, by its length: inflated. */
        memset(&f, 0, sizeof(f));
        f.piece = piece;
        {
            int head = snprintf(reply, sizeof reply, "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
                                "Content-Length: %d\r\n\r\n", (int)(sizeof(GZ) - 1));
            memcpy(reply + head, GZ, sizeof(GZ) - 1);
            f.reply = reply;
            f.reply_len = (size_t)head + sizeof(GZ) - 1;
        }
        httpc_init(&h, io, "tiles.example", 1 << 20);
        httpc_get(&h, "/c");
        r = run(&h);
        check(r == HTTPC_DONE && h.body_len == 23 && memcmp(h.body, "a vector tile, inflated", 23) == 0,
              "a gzipped body was not inflated");
        httpc_free(&h);

        /* 4. No length: the body ends with the connection, which is then
         * not reused. */
        memset(&f, 0, sizeof(f));
        f.piece = piece;
        f.over = 1;
        f.reply = "HTTP/1.0 200 OK\r\n\r\nuntil the end";
        f.reply_len = strlen(f.reply);
        httpc_init(&h, io, "x", 1 << 20);
        httpc_get(&h, "/d");
        r = run(&h);
        check(r == HTTPC_DONE && h.body_len == 13 && !httpc_reusable(&h),
              "a body ended by its connection was not whole, or the connection was kept");
        httpc_free(&h);

        /* 5. A 404 is an answer, not a failure. */
        memset(&f, 0, sizeof(f));
        f.piece = piece;
        f.reply = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        f.reply_len = strlen(f.reply);
        httpc_init(&h, io, "x", 1 << 20);
        httpc_get(&h, "/e");
        r = run(&h);
        check(r == HTTPC_DONE && h.status == 404 && h.body_len == 0, "a 404 was not answered as one");
        httpc_free(&h);

        /* 6. Larger than allowed: refused. */
        memset(&f, 0, sizeof(f));
        f.piece = piece;
        f.reply = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world";
        f.reply_len = strlen(f.reply);
        httpc_init(&h, io, "x", 5);
        httpc_get(&h, "/f");
        check(run(&h) == HTTPC_FAILED, "a body larger than allowed was not refused");
        httpc_free(&h);

        /* 7. Cut short: failed, not done. */
        memset(&f, 0, sizeof(f));
        f.piece = piece;
        f.over = 1;
        f.reply = "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nonly some";
        f.reply_len = strlen(f.reply);
        httpc_init(&h, io, "x", 1 << 20);
        httpc_get(&h, "/g");
        check(run(&h) == HTTPC_FAILED, "a body cut short was taken as whole");
        httpc_free(&h);

        /* 8. Streamed: a 206 from byte 6, into a sink of seven bytes' room
         * that fills, the Range asked for and gzip not. */
        {
            struct pail pl;
            struct httpc_sink sink = { &pl, pail_room, pail_put };
            const char *body = "a model's bytes, a few at a time, into a region that fills";
            size_t blen = strlen(body);

            memset(&pl, 0, sizeof pl);
            pl.cap = sizeof pl.room_for;
            memset(&f, 0, sizeof(f));
            f.piece = piece;
            snprintf(reply, sizeof reply, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes "
                     "6-%zu/%zu\r\nContent-Length: %zu\r\n\r\n%s", blen + 5, blen + 6, blen, body);
            f.reply = reply;
            f.reply_len = strlen(reply);
            httpc_init(&h, io, "models.example", 16);
            check(httpc_get_into(&h, "/m.gguf", 6, sink) == 0, "a streamed GET was refused");
            r = run_into(&h, &pl);

            size_t vlen = 0;
            const char *v = httpc_header(&h, "content-range", &vlen);

            snprintf(said, sizeof said, "in pieces of %zu, a streamed 206: %d %d %zu %.*s", piece, r,
                     h.status, pl.all_len, (int)pl.all_len, pl.all);
            check(r == HTTPC_DONE && h.status == 206 && pl.all_len == blen
                  && memcmp(pl.all, body, blen) == 0 && h.body_len == 0 && h.got == (long long)blen,
                  said);
            check(pl.full_seen || piece < 7, "a full sink was never asked about");
            check(v != NULL && vlen > 6 && memcmp(v, "bytes 6-", 8) == 0,
                  "the 206's Content-Range could not be read");
            check(strstr(f.wrote, "Range: bytes=6-\r\n") != NULL
                  && strstr(f.wrote, "Accept-Encoding: identity\r\n") != NULL,
                  "a streamed GET did not ask for its Range, or asked for gzip");
            httpc_free(&h);

            /* 9. Streamed in chunks, from the start: no Range. */
            memset(&pl, 0, sizeof pl);
            pl.cap = sizeof pl.room_for;
            memset(&f, 0, sizeof(f));
            f.piece = piece;
            f.reply = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "B\r\nchunked and\r\n9\r\n streamed\r\n0\r\n\r\n";
            f.reply_len = strlen(f.reply);
            httpc_init(&h, io, "x", 16);
            httpc_get_into(&h, "/c.gguf", 0, sink);
            r = run_into(&h, &pl);
            check(r == HTTPC_DONE && pl.all_len == 20 && memcmp(pl.all, "chunked and streamed", 20) == 0
                  && strstr(f.wrote, "Range:") == NULL,
                  "a chunked body was not streamed whole, or a first byte asked for a Range");
            httpc_free(&h);

            /* 10. A redirect while streaming: held, not streamed, and its
             * Location read. */
            memset(&pl, 0, sizeof pl);
            pl.cap = sizeof pl.room_for;
            memset(&f, 0, sizeof(f));
            f.piece = piece;
            f.reply = "HTTP/1.1 302 Found\r\nLocation: https://cdn.example/x/m.gguf?sig=1\r\n"
                      "Content-Length: 5\r\n\r\nmoved";
            f.reply_len = strlen(f.reply);
            httpc_init(&h, io, "x", 64);
            httpc_get_into(&h, "/r.gguf", 0, sink);
            r = run_into(&h, &pl);
            v = httpc_header(&h, "location", &vlen);
            check(r == HTTPC_DONE && h.status == 302 && pl.all_len == 0 && h.body_len == 5
                  && v != NULL && vlen == 34 && memcmp(v, "https://cdn.example/x/m.gguf?sig=1", 34) == 0,
                  "a redirect was streamed, or its Location not read");
            httpc_free(&h);

            /* 11. Gzipped though asked not to be: refused, not saved. */
            memset(&pl, 0, sizeof pl);
            pl.cap = sizeof pl.room_for;
            memset(&f, 0, sizeof(f));
            f.piece = piece;
            f.reply = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 3\r\n\r\nabc";
            f.reply_len = strlen(f.reply);
            httpc_init(&h, io, "x", 64);
            httpc_get_into(&h, "/g.gguf", 0, sink);
            check(run_into(&h, &pl) == HTTPC_FAILED && pl.all_len == 0,
                  "a gzipped download was streamed");
            httpc_free(&h);
        }
    }

    if (fails == 0) {
        printf("PASS: %d checks on HTTP from C (in pieces of 1 to 4096 bytes: a body by its "
               "length and the request, a second request on the kept connection in chunks, "
               "gzip inflated, a body ended by its connection, a 404, too large, cut short; "
               "streamed into a sink that fills - a 206 from a Range, chunks, a redirect held "
               "and its Location read, gzip refused)\n",
               checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on HTTP from C\n", fails, checks + fails);
    return 1;
}
