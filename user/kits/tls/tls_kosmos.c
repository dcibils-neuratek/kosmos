/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The TLS Kit: `use("/Kosmos/Kits/tls")`, a TCP connection made secure.
 *
 * **HTTPS, step 2** (`roadmap.md`, the browser: TLS) - Diego, 30 September:
 * "Go for it". BearSSL 0.6 does the protocol (`runtime/upstream/bearssl`,
 * as released): chosen because it was written for machines like this one -
 * no `malloc`, no system calls, an engine its caller feeds bytes into and
 * takes bytes out of - where OpenSSL is half a million lines built on
 * sockets and files. This file is only the feeding.
 *
 *   local t = tls.client(conn, "example.com")      -- a Network Kit conn
 *   t:write("GET / HTTP/1.1\r\n...")               -- bytes it took
 *   t:flush()
 *   t:read()                                       -- plaintext, or nil
 *   t:state()                                      -- "handshake", "open",
 *                                                  -- or "closed", and why
 *   t:trusted()                                    -- true, or false and why
 *   t:resumed()                                    -- the session taken back
 *
 * Nothing here blocks. Each call moves what can move - records to the
 * connection, records from it - and returns; a caller waits on the
 * connection itself (`conn:wait`, `fs.poll`) and asks again, as it would
 * for plain TCP. The engine keeps everything else.
 *
 * **Whom it trusts**: Mozilla's roots as curl publishes them
 * (`assets/ca/cacert.pem`), turned into BearSSL's anchors when the image is
 * built (`build/gen/tls_anchors.c`, by BearSSL's own `brssl ta`), plus any a
 * caller hands in as DER - a test's own authority, or a person's. The
 * certificate's dates are held to the machine's clock - and a caller may
 * ask for the connection **whatever the certificate says**, `insecure =
 * true`, which is the browser's Open anyway. **Its randomness**
 * comes from the hardware through the kernel's health test (`SYS_ENTROPY`):
 * a machine with none gets an error, not a connection.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "bearssl.h"
#include "kosmos.h"

/* The anchors the image carries, generated at build time. */
#include "tls_anchors.c"

#define TLS_MT      "kosmos.tls"
#define EXTRA_MAX   4

struct extra_anchor {
    unsigned char dn[1024];
    size_t        dn_len;
    unsigned char key[1100];        /* RSA-4096's modulus and exponent fit */
};

/*
 * **Open anyway** (`roadmap.md` 6zz c) - Diego, on the browser's drawing:
 * "add a button that says 'open anyways' as well so i can browse it
 * anyway". The chain is still checked, by the same engine against the same
 * roots and the same clock, and its verdict kept for `t:trusted()` to say;
 * what changes is that a chain which fails is taken, the handshake done
 * with the key in the server's own certificate. Nothing here remembers that
 * a host was let through - that is the caller's to decide, per connection.
 */
struct anyway {
    const br_x509_class     *vtable;
    br_x509_minimal_context *check;     /* the real one, for its verdict */
    br_x509_decoder_context  leaf;      /* the server's own certificate */
    int                      certs;     /* how many have begun */
    int                      verdict;   /* what `check` said; -1 before */
};

struct tls {
    br_ssl_client_context  sc;
    br_x509_minimal_context xc;
    struct anyway          anyway;
    int                    insecure;

    /* The session offered back to the server, if one was, and whether this
     * connection's own has been kept for the next (`sessions` below). */
    char                   name[256];
    unsigned char          offered[32];
    unsigned char          offered_len;
    int                    kept;
    unsigned char          iobuf[BR_SSL_BUFSIZE_BIDI];

    br_x509_trust_anchor   anchors[TAs_NUM + EXTRA_MAX];
    struct extra_anchor    extra[EXTRA_MAX];

    int                    conn;            /* the connection, in the registry */

    /* What the connection gave that the engine had no room for yet. */
    unsigned char          pending[16384];
    size_t                 pending_at, pending_len;
};

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
static int anchor_from_der(struct extra_anchor *e, br_x509_trust_anchor *ta,
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
 * A chain checked, and taken whatever the check said.
 *----------------------------------------------------------------------*/

static struct anyway *anyway_of(const br_x509_class **ctx)
{
    return (struct anyway *)(void *)ctx;
}

static void aw_start_chain(const br_x509_class **ctx, const char *server_name)
{
    struct anyway *a = anyway_of(ctx);

    a->check->vtable->start_chain(&a->check->vtable, server_name);
    br_x509_decoder_init(&a->leaf, NULL, NULL);
    a->certs = 0;
    a->verdict = -1;
}

static void aw_start_cert(const br_x509_class **ctx, uint32_t length)
{
    struct anyway *a = anyway_of(ctx);

    a->check->vtable->start_cert(&a->check->vtable, length);
    a->certs++;
}

/* Every certificate to the check; the first, the server's, to the decoder
 * as well, for its key. */
static void aw_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    struct anyway *a = anyway_of(ctx);

    a->check->vtable->append(&a->check->vtable, buf, len);

    if (a->certs == 1) {
        br_x509_decoder_push(&a->leaf, buf, len);
    }
}

static void aw_end_cert(const br_x509_class **ctx)
{
    struct anyway *a = anyway_of(ctx);

    a->check->vtable->end_cert(&a->check->vtable);
}

static unsigned aw_end_chain(const br_x509_class **ctx)
{
    struct anyway *a = anyway_of(ctx);

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
    struct anyway *a = (struct anyway *)(void *)ctx;
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
    sizeof(struct anyway),
    aw_start_chain,
    aw_start_cert,
    aw_append,
    aw_end_cert,
    aw_end_chain,
    aw_get_pkey
};

/*------------------------------------------------------------------------
 * The connection, reached through its own methods.
 *----------------------------------------------------------------------*/

/* `conn:write(bytes)`: how many it took. */
static size_t conn_write(lua_State *L, struct tls *t, const unsigned char *p, size_t n)
{
    lua_Integer wrote;

    lua_rawgeti(L, LUA_REGISTRYINDEX, t->conn);
    lua_getfield(L, -1, "write");
    lua_pushvalue(L, -2);
    lua_pushlstring(L, (const char *)p, n);
    lua_call(L, 2, 1);
    wrote = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
    lua_pop(L, 2);

    return wrote > 0 ? (size_t)wrote : 0;
}

/* `conn:read()`, into `pending`, when it is empty. 1 when bytes came. */
static int conn_read(lua_State *L, struct tls *t)
{
    size_t n = 0;
    const char *s;
    int got = 0;

    if (t->pending_at < t->pending_len) {
        return 1;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, t->conn);
    lua_getfield(L, -1, "read");
    lua_pushvalue(L, -2);
    lua_call(L, 1, 1);

    s = lua_tolstring(L, -1, &n);

    if (s != NULL && n > 0) {
        if (n > sizeof(t->pending)) {
            n = sizeof(t->pending);         /* a ring holds no more than this */
        }

        memcpy(t->pending, s, n);
        t->pending_at = 0;
        t->pending_len = n;
        got = 1;
    }

    lua_pop(L, 2);
    return got;
}

/*
 * Everything that can move, moved: the engine's records to the connection
 * while it takes them, the connection's bytes to the engine while it has
 * room. Returns when neither can move.
 */
static void pump(lua_State *L, struct tls *t)
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
            size_t wrote = conn_write(L, t, buf, len);

            if (wrote > 0) {
                br_ssl_engine_sendrec_ack(eng, wrote);
                moved = 1;
            }
        }

        if (state & BR_SSL_RECVREC) {
            if (conn_read(L, t)) {
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

static struct tls *check_tls(lua_State *L)
{
    return luaL_checkudata(L, 1, TLS_MT);
}

/*------------------------------------------------------------------------
 * What Lua sees.
 *----------------------------------------------------------------------*/

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
static void session_keep(struct tls *t)
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

/*
 * `tls.client(conn, name [, { anchors = { der, ... }, insecure = true }])` -
 * the handshake begun on a connection that is already open; `name` is both
 * the name the certificate must carry and the one sent in SNI. `insecure`
 * checks the certificate all the same and goes on whatever it finds.
 */
static int l_client(lua_State *L)
{
    const char *name = luaL_checkstring(L, 2);
    struct tls *t;
    size_t n = TAs_NUM;
    unsigned char seed[32];
    struct sysinfo info;

    luaL_checkany(L, 1);

    t = lua_newuserdatauv(L, sizeof(*t), 0);
    memset(t, 0, sizeof(*t));
    luaL_setmetatable(L, TLS_MT);

    memcpy(t->anchors, TAs, sizeof(TAs));

    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "anchors");

        if (lua_istable(L, -1)) {
            lua_Integer i;

            for (i = 1; i <= (lua_Integer)luaL_len(L, -1) && n < TAs_NUM + EXTRA_MAX; i++) {
                size_t len;
                const char *der;

                lua_rawgeti(L, -1, i);
                der = luaL_checklstring(L, -1, &len);

                if (!anchor_from_der(&t->extra[n - TAs_NUM], &t->anchors[n],
                                     (const unsigned char *)der, len)) {
                    return luaL_error(L, "tls.client: anchor %d is not a certificate "
                                      "this can use", (int)i);
                }

                n++;
                lua_pop(L, 1);
            }
        }

        lua_pop(L, 1);

        lua_getfield(L, 3, "insecure");
        t->insecure = lua_toboolean(L, -1);
        lua_pop(L, 1);
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
        return luaL_error(L, "tls.client: this machine has no source of randomness, "
                          "and TLS without it would not be secure");
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
        return luaL_error(L, "tls.client: %s", error_name(br_ssl_engine_last_error(&t->sc.eng)));
    }

    lua_pushvalue(L, 1);
    t->conn = luaL_ref(L, LUA_REGISTRYINDEX);

    pump(L, t);
    return 1;
}

/* `t:write(bytes)` - how many the engine took; 0 until the handshake is done. */
static int l_write(lua_State *L)
{
    struct tls *t = check_tls(L);
    size_t len, room, take = 0;
    const char *s = luaL_checklstring(L, 2, &len);

    pump(L, t);
    session_keep(t);

    if (br_ssl_engine_current_state(&t->sc.eng) & BR_SSL_SENDAPP) {
        unsigned char *buf = br_ssl_engine_sendapp_buf(&t->sc.eng, &room);

        take = len < room ? len : room;
        memcpy(buf, s, take);
        br_ssl_engine_sendapp_ack(&t->sc.eng, take);
        pump(L, t);
    }

    lua_pushinteger(L, (lua_Integer)take);
    return 1;
}

/* `t:flush()` - what was written, sent now rather than when a record fills. */
static int l_flush(lua_State *L)
{
    struct tls *t = check_tls(L);

    br_ssl_engine_flush(&t->sc.eng, 0);
    pump(L, t);
    return 0;
}

/* `t:read()` - the plaintext that has arrived, or nil. */
static int l_read(lua_State *L)
{
    struct tls *t = check_tls(L);

    pump(L, t);
    session_keep(t);

    if (br_ssl_engine_current_state(&t->sc.eng) & BR_SSL_RECVAPP) {
        size_t len;
        unsigned char *buf = br_ssl_engine_recvapp_buf(&t->sc.eng, &len);

        lua_pushlstring(L, (const char *)buf, len);
        br_ssl_engine_recvapp_ack(&t->sc.eng, len);
        pump(L, t);
        return 1;
    }

    lua_pushnil(L);
    return 1;
}

/* `t:state()` - "handshake", "open" or "closed", and when closed, why, the
 * error's number, and whether it was the certificate. */
static int l_state(lua_State *L)
{
    struct tls *t = check_tls(L);
    unsigned state;

    pump(L, t);
    session_keep(t);
    state = br_ssl_engine_current_state(&t->sc.eng);

    if (state & BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(&t->sc.eng);

        lua_pushstring(L, "closed");
        lua_pushstring(L, error_name(err));
        lua_pushinteger(L, err);
        lua_pushboolean(L, certificate_error(err));
        return 4;
    }

    lua_pushstring(L, (state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) ? "open" : "handshake");
    return 1;
}

/*
 * `t:trusted()` - true when the certificate checked out; false and why when
 * it did not, which only a connection asked for `insecure` gets as far as
 * being open with; nil while nobody knows yet.
 */
static int l_trusted(lua_State *L)
{
    struct tls *t = check_tls(L);
    unsigned state;

    pump(L, t);
    state = br_ssl_engine_current_state(&t->sc.eng);

    if (t->insecure && t->anyway.verdict >= 0) {
        if (t->anyway.verdict == 0) {
            lua_pushboolean(L, 1);
            return 1;
        }

        lua_pushboolean(L, 0);
        lua_pushstring(L, error_name(t->anyway.verdict));
        return 2;
    }

    if (!t->insecure && (state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP))) {
        lua_pushboolean(L, 1);
        return 1;
    }

    if (state & BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(&t->sc.eng);

        if (certificate_error(err)) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, error_name(err));
            return 2;
        }
    }

    lua_pushnil(L);
    return 1;
}

/*
 * `t:resumed()` - true when the server took back the session offered, so the
 * key exchange was skipped; false when it made a new one; nil before the
 * handshake is done.
 */
static int l_resumed(lua_State *L)
{
    struct tls *t = check_tls(L);
    br_ssl_session_parameters p;

    pump(L, t);
    session_keep(t);

    if ((br_ssl_engine_current_state(&t->sc.eng) & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) == 0) {
        lua_pushnil(L);
        return 1;
    }

    br_ssl_engine_get_session_parameters(&t->sc.eng, &p);
    lua_pushboolean(L, t->offered_len > 0 && p.session_id_len == t->offered_len
                       && memcmp(p.session_id, t->offered, t->offered_len) == 0);
    return 1;
}

/* `t:close()` - a close_notify, sent. The connection is the caller's. */
static int l_close(lua_State *L)
{
    struct tls *t = check_tls(L);

    br_ssl_engine_close(&t->sc.eng);
    pump(L, t);
    return 0;
}

static int l_gc(lua_State *L)
{
    struct tls *t = check_tls(L);

    if (t->conn != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, t->conn);
        t->conn = 0;
    }

    return 0;
}

void kosmos_tls_kit(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "write", l_write },
        { "flush", l_flush },
        { "read",  l_read },
        { "state", l_state },
        { "trusted", l_trusted },
        { "resumed", l_resumed },
        { "close", l_close },
        { "__gc",  l_gc },
        { NULL, NULL }
    };
    static const luaL_Reg api[] = {
        { "client", l_client },
        { NULL, NULL }
    };

    luaL_newmetatable(L, TLS_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pop(L, 1);

    luaL_newlib(L, api);
    lua_pushinteger(L, TAs_NUM);
    lua_setfield(L, -2, "anchors");
}
