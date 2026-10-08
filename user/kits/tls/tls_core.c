/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **TLS's core, for C and Lua alike** (`tls_core.h`, `docs/maps.md` M6a).
 *
 * BearSSL 0.6 does the protocol (`runtime/upstream/bearssl`, as released):
 * chosen because it was written for machines like this one - no `malloc`,
 * no system calls, an engine its caller feeds bytes into and takes bytes
 * out of. This is the feeding, and what the TLS Kit decided about it on 30
 * September: whom to trust, the clock, the randomness, Open anyway, and the
 * sessions kept to be offered back.
 *
 * **It was the Lua kit's alone** (`tls_kosmos.c`), its bytes moved by
 * calling a Lua connection's `read` and `write`. A server in C - the map's
 * `tiles` - needs exactly the same and has no Lua, so the core came out of
 * the binding: bytes move through `struct tls_io`'s two calls, and the Lua
 * kit is the first caller of it rather than its only home.
 *
 * **Whom it trusts**: Mozilla's roots as curl publishes them
 * (`assets/ca/cacert.pem`), made BearSSL's anchors when the image is built
 * (`build/host/tls_anchors.c`, by BearSSL's own `brssl ta`), plus any a
 * caller hands in as DER. The certificate's dates are held to the machine's
 * clock, and a caller may ask for the connection **whatever the certificate
 * says** (`insecure`, the browser's Open anyway). **Its randomness** comes
 * from the hardware through the kernel's health test (`SYS_ENTROPY`): a
 * machine with none gets an error, not a connection.
 */

#include "tls_core.h"

#include <stdlib.h>
#include <string.h>

#include "kosmos.h"

/* The anchors the image carries, generated at build time. */
#include "tls_anchors.c"

size_t tls_core_anchors(void)
{
    return TAs_NUM;
}

/*------------------------------------------------------------------------
 * An extra anchor, from a certificate in DER.
 *----------------------------------------------------------------------*/

struct dn_sink { unsigned char *buf; size_t len, max; int over; };

static void append_dn(void *ctx, const void *buf, size_t len)
{
    struct dn_sink *s = ctx;

    if (s->len + len > s->max) {
        s->over = 1;
        return;
    }

    memcpy(s->buf + s->len, buf, len);
    s->len += len;
}

/* The certificate's subject and key as an anchor; 0 when it will not do. */
static int anchor_from_der(struct tls_extra_anchor *e, br_x509_trust_anchor *ta,
                           const unsigned char *der, size_t len)
{
    br_x509_decoder_context dc;
    struct dn_sink sink = { e->dn, 0, sizeof(e->dn), 0 };
    br_x509_pkey *pk;

    br_x509_decoder_init(&dc, append_dn, &sink);
    br_x509_decoder_push(&dc, der, len);
    pk = br_x509_decoder_get_pkey(&dc);

    if (pk == NULL || sink.over || br_x509_decoder_last_error(&dc) != 0) {
        return 0;
    }

    e->dn_len = sink.len;
    ta->dn.data = e->dn;
    ta->dn.len = e->dn_len;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    ta->pkey = *pk;

    /* The key's bytes live in the decoder, which is about to go: copied. */
    if (pk->key_type == BR_KEYTYPE_RSA) {
        size_t nl = pk->key.rsa.nlen, el = pk->key.rsa.elen;

        if (nl + el > sizeof(e->key)) {
            return 0;
        }

        memcpy(e->key, pk->key.rsa.n, nl);
        memcpy(e->key + nl, pk->key.rsa.e, el);
        ta->pkey.key.rsa.n = e->key;
        ta->pkey.key.rsa.e = e->key + nl;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        size_t ql = pk->key.ec.qlen;

        if (ql > sizeof(e->key)) {
            return 0;
        }

        memcpy(e->key, pk->key.ec.q, ql);
        ta->pkey.key.ec.q = e->key;
    } else {
        return 0;
    }

    return 1;
}

/*------------------------------------------------------------------------
 * **Open anyway** (`roadmap.md` 6zz c) - Diego, on the browser's drawing:
 * "add a button that says 'open anyways' as well so i can browse it
 * anyway". The chain is still checked, by the same engine against the same
 * roots and the same clock, and its verdict kept for `tls_core_trusted`;
 * what changes is that a chain which fails is taken, the handshake done
 * with the key in the server's own certificate. Nothing here remembers that
 * a host was let through - that is the caller's to decide, per connection.
 *----------------------------------------------------------------------*/

static struct tls_anyway *anyway_of(const br_x509_class **ctx)
{
    return (struct tls_anyway *)(void *)ctx;
}

static void aw_start_chain(const br_x509_class **ctx, const char *server_name)
{
    struct tls_anyway *a = anyway_of(ctx);

    a->check->vtable->start_chain(&a->check->vtable, server_name);
    br_x509_decoder_init(&a->leaf, NULL, NULL);
    a->certs = 0;
    a->verdict = -1;
}

static void aw_start_cert(const br_x509_class **ctx, uint32_t length)
{
    struct tls_anyway *a = anyway_of(ctx);

    a->check->vtable->start_cert(&a->check->vtable, length);
    a->certs++;
}

/* Every certificate to the check; the first, the server's, to the decoder
 * as well, for its key. */
static void aw_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    struct tls_anyway *a = anyway_of(ctx);

    a->check->vtable->append(&a->check->vtable, buf, len);

    if (a->certs == 1) {
        br_x509_decoder_push(&a->leaf, buf, len);
    }
}

static void aw_end_cert(const br_x509_class **ctx)
{
    struct tls_anyway *a = anyway_of(ctx);

    a->check->vtable->end_cert(&a->check->vtable);
}

static unsigned aw_end_chain(const br_x509_class **ctx)
{
    struct tls_anyway *a = anyway_of(ctx);

    a->verdict = (int)a->check->vtable->end_chain(&a->check->vtable);

    /* A chain whose server certificate could not even be read has no key to
     * go on with, so that one is refused whatever was asked. */
    if (a->verdict != 0 && br_x509_decoder_get_pkey(&a->leaf) == NULL) {
        return (unsigned)a->verdict;
    }

    return 0;
}

static const br_x509_pkey *aw_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    struct tls_anyway *a = (struct tls_anyway *)(void *)ctx;
    const br_x509_pkey *pk = a->check->vtable->get_pkey(&a->check->vtable, usages);

    /* The check hands its key over only for a chain it accepted, or one
     * that fails only for want of a root. Otherwise, the server's own. */
    if (pk != NULL) {
        return pk;
    }

    if (usages != NULL) {
        *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    }

    return br_x509_decoder_get_pkey(&a->leaf);
}

static const br_x509_class anyway_class = {
    sizeof(struct tls_anyway),
    aw_start_chain,
    aw_start_cert,
    aw_append,
    aw_end_cert,
    aw_end_chain,
    aw_get_pkey
};

/*------------------------------------------------------------------------
 * The connection, through the caller's two calls.
 *----------------------------------------------------------------------*/

/* Bytes from the connection into `pending`, when it is empty. 1 when there
 * are some. */
static int conn_read(struct tls_conn *t)
{
    size_t n;

    if (t->pending_at < t->pending_len) {
        return 1;
    }

    n = t->io.read(t->io.user, t->pending, sizeof(t->pending));

    if (n == 0) {
        return 0;
    }

    t->pending_at = 0;
    t->pending_len = n;
    return 1;
}

/*
 * Everything that can move, moved: the engine's records to the connection
 * while it takes them, the connection's bytes to the engine while it has
 * room. Returns when neither can move.
 */
static void pump(struct tls_conn *t)
{
    br_ssl_engine_context *eng = &t->sc.eng;

    for (;;) {
        unsigned state = br_ssl_engine_current_state(eng);
        int moved = 0;

        if (state & BR_SSL_CLOSED) {
            return;
        }

        if (state & BR_SSL_SENDREC) {
            size_t len;
            unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
            size_t wrote = t->io.write(t->io.user, buf, len);

            if (wrote > 0) {
                br_ssl_engine_sendrec_ack(eng, wrote);
                moved = 1;
            }
        }

        if (state & BR_SSL_RECVREC) {
            if (conn_read(t)) {
                size_t room, take;
                unsigned char *buf = br_ssl_engine_recvrec_buf(eng, &room);

                take = t->pending_len - t->pending_at;
                take = take < room ? take : room;
                memcpy(buf, t->pending + t->pending_at, take);
                t->pending_at += take;
                br_ssl_engine_recvrec_ack(eng, take);
                moved = 1;
            }
        }

        if (!moved) {
            return;
        }
    }
}

/* A name for what went wrong, where it is one a person meets. */
static const char *error_name(int err)
{
    switch (err) {
    case BR_ERR_OK:                   return "none";
    case BR_ERR_X509_NOT_TRUSTED:     return "the certificate is signed by nobody this machine trusts";
    case BR_ERR_X509_EXPIRED:         return "the certificate is out of its dates";
    case BR_ERR_X509_BAD_SERVER_NAME: return "the certificate is for another name";
    case BR_ERR_X509_BAD_SIGNATURE:   return "the certificate's signature is wrong";
    case BR_ERR_X509_NOT_CA:          return "a certificate in the chain is not an authority";
    case BR_ERR_UNSUPPORTED_VERSION:  return "the server speaks no TLS version this does";
    case BR_ERR_BAD_HANDSHAKE:        return "the handshake went wrong";
    case BR_ERR_NO_RANDOM:            return "there was no randomness";
    case BR_ERR_IO:                   return "the connection failed";
    default:
        if (err >= BR_ERR_SEND_FATAL_ALERT) return "this side sent an alert";
        if (err >= BR_ERR_RECV_FATAL_ALERT) return "the server sent an alert";
        return "a TLS error";
    }
}

/*
 * **Sessions, kept to be offered back** (`roadmap.md` 6zz g). A connection
 * to a host this process has already met offers the session it had, and a
 * server that agrees skips the key exchange - no public-key arithmetic,
 * which was the largest thing gnu.org's twelve pictures cost the browser, and
 * a round trip fewer. BearSSL does the protocol; this keeps the sessions.
 *
 * Kept by the name the certificate was checked against, and **only for a
 * connection whose certificate checked out**: a session opened anyway, whose
 * chain did not, is never kept, since resuming it would take a later
 * connection past the check it was never given. A cache, and bounded as one
 * is: the oldest goes when it is full, as ARP's entries do.
 */
#define SESSIONS 32

static struct {
    char                       name[256];
    br_ssl_session_parameters  params;
    uint32_t                   used;        /* when, for the oldest */
} sessions[SESSIONS];

static uint32_t session_clock;

static int session_find(const char *name)
{
    int i;

    for (i = 0; i < SESSIONS; i++) {
        if (sessions[i].name[0] != '\0' && strcmp(sessions[i].name, name) == 0) {
            return i;
        }
    }

    return -1;
}

/* This connection's session, kept for its name once its handshake is done -
 * the first time it is seen open, and only when it may be. */
static void session_keep(struct tls_conn *t)
{
    br_ssl_session_parameters p;
    int i, oldest = 0;

    if (t->kept || t->insecure || t->name[0] == '\0'
        || (br_ssl_engine_current_state(&t->sc.eng) & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) == 0) {
        return;
    }

    t->kept = 1;
    br_ssl_engine_get_session_parameters(&t->sc.eng, &p);

    if (p.session_id_len == 0) {
        return;                     /* the server offers no resumption */
    }

    i = session_find(t->name);

    if (i < 0) {
        for (i = 0; i < SESSIONS; i++) {
            if (sessions[i].used < sessions[oldest].used) {
                oldest = i;
            }
        }

        i = oldest;
        strcpy(sessions[i].name, t->name);
    }

    sessions[i].params = p;
    sessions[i].used = ++session_clock;
}

/* Whether an error is about the certificate rather than the connection:
 * the kind a person may choose to go past. */
static int certificate_error(int err)
{
    return err > BR_ERR_X509_OK && err <= BR_ERR_X509_NOT_TRUSTED;
}

/*------------------------------------------------------------------------
 * The door.
 *----------------------------------------------------------------------*/

int tls_core_open(struct tls_conn *t, const char *name,
                  const unsigned char *const *ders, const size_t *der_lens, int count,
                  int insecure, struct tls_io io, const char **why)
{
    size_t n = TAs_NUM;
    unsigned char seed[32];
    struct sysinfo info;

    memset(t, 0, sizeof(*t));
    t->io = io;
    t->insecure = insecure;

    if (count > TLS_EXTRA_MAX) {
        *why = "more authorities than a connection takes";
        return -1;
    }

    t->anchors = malloc((TAs_NUM + (size_t)count) * sizeof(*t->anchors));

    if (t->anchors == NULL) {
        *why = "no memory for the authorities";
        return -1;
    }

    memcpy(t->anchors, TAs, sizeof(TAs));

    for (int i = 0; i < count; i++) {
        if (!anchor_from_der(&t->extra[i], &t->anchors[n], ders[i], der_lens[i])) {
            *why = "an authority given is not a certificate this can use";
            tls_core_free(t);
            return -1;
        }

        n++;
    }

    br_ssl_client_init_full(&t->sc, &t->xc, t->anchors, n);

    if (t->insecure) {
        t->anyway.vtable = &anyway_class;
        t->anyway.check = &t->xc;
        t->anyway.verdict = -1;
        br_ssl_engine_set_x509(&t->sc.eng, &t->anyway.vtable);
    }

    br_ssl_engine_set_buffer(&t->sc.eng, t->iobuf, sizeof(t->iobuf), 1);

    /* The certificate's dates against this machine's clock: days since 1
     * January of year 0, and seconds into the day. */
    if (kosmos_sysinfo(&info) == 0 && info.epoch > 0) {
        br_x509_minimal_set_time(&t->xc, (uint32_t)(info.epoch / 86400u + 719528u),
                                 (uint32_t)(info.epoch % 86400u));
    }

    if (kosmos_entropy(seed, sizeof(seed)) != (long)sizeof(seed)) {
        *why = "this machine has no source of randomness, and TLS without it would not be secure";
        tls_core_free(t);
        return -1;
    }

    br_ssl_engine_inject_entropy(&t->sc.eng, seed, sizeof(seed));
    memset(seed, 0, sizeof(seed));

    /* A session this process kept for the name, offered back - never for a
     * connection going on whatever its certificate says. */
    {
        int kept = t->insecure ? -1 : session_find(name);

        if (strlen(name) < sizeof(t->name)) {
            strcpy(t->name, name);
        }

        if (kept >= 0) {
            br_ssl_engine_set_session_parameters(&t->sc.eng, &sessions[kept].params);
            memcpy(t->offered, sessions[kept].params.session_id,
                   sessions[kept].params.session_id_len);
            t->offered_len = sessions[kept].params.session_id_len;
            sessions[kept].used = ++session_clock;
        }
    }

    if (!br_ssl_client_reset(&t->sc, name, t->offered_len > 0)) {
        *why = error_name(br_ssl_engine_last_error(&t->sc.eng));
        tls_core_free(t);
        return -1;
    }

    pump(t);
    return 0;
}

void tls_core_free(struct tls_conn *t)
{
    free(t->anchors);
    t->anchors = NULL;
}

size_t tls_core_write(struct tls_conn *t, const void *p, size_t n)
{
    size_t room, take = 0;

    pump(t);
    session_keep(t);

    if (br_ssl_engine_current_state(&t->sc.eng) & BR_SSL_SENDAPP) {
        unsigned char *buf = br_ssl_engine_sendapp_buf(&t->sc.eng, &room);

        take = n < room ? n : room;
        memcpy(buf, p, take);
        br_ssl_engine_sendapp_ack(&t->sc.eng, take);
        pump(t);
    }

    return take;
}

void tls_core_flush(struct tls_conn *t)
{
    br_ssl_engine_flush(&t->sc.eng, 0);
    pump(t);
}

const unsigned char *tls_core_peek(struct tls_conn *t, size_t *len)
{
    pump(t);
    session_keep(t);

    if (br_ssl_engine_current_state(&t->sc.eng) & BR_SSL_RECVAPP) {
        return br_ssl_engine_recvapp_buf(&t->sc.eng, len);
    }

    *len = 0;
    return NULL;
}

void tls_core_take(struct tls_conn *t, size_t len)
{
    br_ssl_engine_recvapp_ack(&t->sc.eng, len);
    pump(t);
}

size_t tls_core_read(struct tls_conn *t, void *buf, size_t max)
{
    size_t len;
    const unsigned char *p = tls_core_peek(t, &len);

    if (p == NULL) {
        return 0;
    }

    if (len > max) {
        len = max;
    }

    memcpy(buf, p, len);
    tls_core_take(t, len);
    return len;
}

int tls_core_state(struct tls_conn *t, const char **why, int *err, int *certificate)
{
    unsigned state;

    pump(t);
    session_keep(t);
    state = br_ssl_engine_current_state(&t->sc.eng);

    if (state & BR_SSL_CLOSED) {
        int e = br_ssl_engine_last_error(&t->sc.eng);

        if (why) *why = error_name(e);
        if (err) *err = e;
        if (certificate) *certificate = certificate_error(e);
        return TLS_CLOSED;
    }

    return (state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) ? TLS_OPEN : TLS_HANDSHAKE;
}

int tls_core_trusted(struct tls_conn *t, const char **why)
{
    unsigned state;

    pump(t);
    state = br_ssl_engine_current_state(&t->sc.eng);

    if (t->insecure && t->anyway.verdict >= 0) {
        if (t->anyway.verdict == 0) {
            return 1;
        }

        *why = error_name(t->anyway.verdict);
        return 0;
    }

    if (!t->insecure && (state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP))) {
        return 1;
    }

    if (state & BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(&t->sc.eng);

        if (certificate_error(err)) {
            *why = error_name(err);
            return 0;
        }
    }

    return -1;
}

int tls_core_resumed(struct tls_conn *t)
{
    br_ssl_session_parameters p;

    pump(t);
    session_keep(t);

    if ((br_ssl_engine_current_state(&t->sc.eng) & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) == 0) {
        return -1;
    }

    br_ssl_engine_get_session_parameters(&t->sc.eng, &p);
    return t->offered_len > 0 && p.session_id_len == t->offered_len
           && memcmp(p.session_id, t->offered, t->offered_len) == 0;
}

void tls_core_close(struct tls_conn *t)
{
    br_ssl_engine_close(&t->sc.eng);
    pump(t);
}
