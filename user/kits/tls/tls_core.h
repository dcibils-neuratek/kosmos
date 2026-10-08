/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * TLS from C: a connection made secure, for whoever moves its bytes
 * (`tls_core.c`, `docs/maps.md` M6a). The TLS Kit's Lua door
 * (`tls_kosmos.c`) is one caller, moving bytes through a Lua connection;
 * a C server is another, moving them through the Network Kit's C client.
 *
 * Nothing here blocks. Each call moves what can move through `io` and
 * returns; a caller waits on its connection and asks again.
 */

#ifndef KOSMOS_TLS_CORE_H
#define KOSMOS_TLS_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "bearssl.h"

/* How bytes reach the connection and come from it, the caller's: `write`
 * answers how many it took, `read` how many it put at `buf` (0 for none
 * yet). Neither waits. */
struct tls_io {
    void   *user;
    size_t (*write)(void *user, const unsigned char *p, size_t n);
    size_t (*read)(void *user, unsigned char *buf, size_t max);
};

#define TLS_EXTRA_MAX 4

struct tls_extra_anchor {
    unsigned char dn[1024];
    size_t        dn_len;
    unsigned char key[1100];        /* RSA-4096's modulus and exponent fit */
};

/* A chain checked, and taken whatever the check said (Open anyway). */
struct tls_anyway {
    const br_x509_class     *vtable;
    br_x509_minimal_context *check;
    br_x509_decoder_context  leaf;
    int                      certs;
    int                      verdict;   /* what `check` said; -1 before */
};

struct tls_conn {
    br_ssl_client_context   sc;
    br_x509_minimal_context xc;
    struct tls_anyway       anyway;
    int                     insecure;
    struct tls_io           io;

    char                    name[256];
    unsigned char           offered[32];
    unsigned char           offered_len;
    int                     kept;
    unsigned char           iobuf[BR_SSL_BUFSIZE_BIDI];

    br_x509_trust_anchor   *anchors;    /* the image's and the caller's */
    struct tls_extra_anchor extra[TLS_EXTRA_MAX];

    /* What the connection gave that the engine had no room for yet. */
    unsigned char           pending[16384];
    size_t                  pending_at, pending_len;
};

enum { TLS_HANDSHAKE = 0, TLS_OPEN = 1, TLS_CLOSED = 2 };

/*
 * The handshake begun on a connection already open: `name` both the name
 * the certificate must carry and the one sent; `ders` extra authorities in
 * DER, `count` of them, at most `TLS_EXTRA_MAX`; `insecure` checks the
 * certificate all the same and goes on whatever it finds. 0, or -1 with
 * `*why` saying why. `t` is the caller's, large (about 40 KB): a static or
 * pages, not a stack.
 */
int tls_core_open(struct tls_conn *t, const char *name,
                  const unsigned char *const *ders, const size_t *der_lens, int count,
                  int insecure, struct tls_io io, const char **why);

/* Plaintext in: how many bytes the engine took, 0 until it is open. */
size_t tls_core_write(struct tls_conn *t, const void *p, size_t n);

/* What was written, sent now rather than when a record fills. */
void tls_core_flush(struct tls_conn *t);

/* Plaintext out: up to `max` bytes into `buf`, how many; 0 for none yet. */
size_t tls_core_read(struct tls_conn *t, void *buf, size_t max);

/* The plaintext that has arrived, in place - `*len` of it, NULL for none -
 * and taken once the caller has it (`tls_core_take`). The Lua door's way,
 * which makes one string of it with no copy between. */
const unsigned char *tls_core_peek(struct tls_conn *t, size_t *len);
void tls_core_take(struct tls_conn *t, size_t len);

/* TLS_HANDSHAKE, TLS_OPEN or TLS_CLOSED; when closed, why, the error's
 * number and whether it was the certificate, through the pointers given. */
int tls_core_state(struct tls_conn *t, const char **why, int *err, int *certificate);

/* 1 when the certificate checked out; 0 and why when it did not (only an
 * `insecure` connection gets as far as open with that); -1 not known yet. */
int tls_core_trusted(struct tls_conn *t, const char **why);

/* 1 when the server took back the session offered; 0 a new one; -1 before. */
int tls_core_resumed(struct tls_conn *t);

/* A close_notify, sent. The connection is the caller's. */
void tls_core_close(struct tls_conn *t);

/* What `tls_core_open` took (its list of authorities), given back. */
void tls_core_free(struct tls_conn *t);

/* How many authorities the image carries. */
size_t tls_core_anchors(void);

#endif
