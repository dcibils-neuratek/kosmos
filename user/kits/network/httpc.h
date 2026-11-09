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

/*
 * **A body streamed rather than held** (`docs/xavier.md`, *Models*): a
 * model is 1.6 GB, and nothing should hold that in memory. With a sink, a
 * 200's or a 206's body goes to `put` as it comes, never more at once than
 * `room` says there is space for - so a sink that is full stops the reading
 * until it has been emptied, and nothing is lost. Any other status - a
 * redirect, a refusal - is held as before, since it is short and its words
 * are wanted. A streamed body is never gzipped: the request says so.
 */
struct httpc_sink {
    void   *user;
    size_t (*room)(void *user);
    int    (*put)(void *user, const uint8_t *p, size_t n);   /* 0: refused */
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
    long long got;                  /* the body's bytes so far, held or streamed */
    struct httpc_sink sink;         /* `put` NULL: held */

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

/* The same, streamed into `sink`, from byte `from` of the file (a Range,
 * when it is not the first): the reply's 206 says the server took it. */
int  httpc_get_into(struct httpc *h, const char *path, long long from, struct httpc_sink sink);

/* A header of the reply's head, its name's case not minded: its value and
 * `*len`, or NULL. A redirect's Location, a 206's Content-Range. */
const char *httpc_header(const struct httpc *h, const char *name, size_t *len);

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
