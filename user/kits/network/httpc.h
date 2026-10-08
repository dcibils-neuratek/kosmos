/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * HTTP from C (`httpc.c`, `docs/maps.md` M6c): GET requests on one
 * connection, stepped - a server drives as many as it likes and waits for
 * none. The bytes move through two calls the caller gives, so the same
 * client speaks over the Network Kit's plain TCP, over the TLS core, and on
 * the Mac against a test's replies.
 */

#ifndef KOSMOS_HTTPC_H
#define KOSMOS_HTTPC_H

#include <stddef.h>
#include <stdint.h>

/* How bytes move: `write` answers how many it took, -1 for a connection
 * that is over; `read` how many it put at `buf`, 0 for none yet, -1 for
 * over with nothing left. Neither waits. */
struct httpc_io {
    void *user;
    long (*write)(void *user, const void *p, size_t n);
    long (*read)(void *user, void *buf, size_t max);
};

enum { HTTPC_IDLE, HTTPC_SENDING, HTTPC_HEAD, HTTPC_BODY, HTTPC_DONE, HTTPC_FAILED };

#define HTTPC_HEAD_MAX 16384

struct httpc {
    struct httpc_io io;
    char     host[256];
    int      state;
    const char *why;                /* what went wrong, for HTTPC_FAILED */

    /* The request under way. */
    char     request[1024];
    size_t   request_len, request_sent;

    /* The reply: its status, how its body ends, and the body. */
    int      status;
    long long length;               /* Content-Length, or -1 */
    int      chunked, gzip, closes;
    char     head[HTTPC_HEAD_MAX];
    size_t   head_len;

    uint8_t *body;
    size_t   body_len, body_cap, body_most;

    /* Chunks: bytes left in this one, or -1 when a size line is next. */
    long long chunk_left;
    char     line[32];
    size_t   line_len;
    int      after_chunk;           /* a CRLF after a chunk's data */
};

/* A client over `io` for `host` (sent as the Host header), its body held
 * to `most` bytes. Idle. */
void httpc_init(struct httpc *h, struct httpc_io io, const char *host, size_t most);

/* GET `path` begun on the connection. 0, or -1 when one is under way. */
int  httpc_get(struct httpc *h, const char *path);

/* What can move, moved: HTTPC_SENDING, HEAD or BODY while under way;
 * HTTPC_DONE with `status` and the body (inflated, if it came gzipped);
 * HTTPC_FAILED with `why`. */
int  httpc_step(struct httpc *h);

/* Whether the connection may carry another request after a DONE. */
int  httpc_reusable(const struct httpc *h);

/* The body taken, and the client idle for the next request. */
void httpc_reset(struct httpc *h);

/* What it holds, given back. */
void httpc_free(struct httpc *h);

#endif
