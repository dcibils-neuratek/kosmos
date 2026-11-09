/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A download, into a region** (`docs/xavier.md`, *Models*): one HTTP or
 * HTTPS GET whose body never becomes a Lua string. A model is 1.6 GB; the
 * Lua door of the TLS Kit moves every record through a Lua string, and
 * `http.lua` holds a whole reply in one, which is no way to carry it.
 *
 *   local d = net.download(cap, { address = a, port = 443, host = "huggingface.co",
 *                                 path = "/...", tls = true, from = 0,
 *                                 at = r.at, bytes = r.size, anchors = { der } })
 *   d:step()        "sending", "head", "body", "done", or "failed" and why
 *   d:ready()       bytes waiting at the region's start
 *   d:taken()       the region emptied: written out, and room again
 *   d:status()      the reply's status; d:length() its Content-Length or -1
 *   d:got()         body bytes so far; d:header(name) a header's value
 *   d:trusted()     over TLS: true, or false and why
 *   d:wait(ticks)   block until the connection has something, or ticks pass
 *   d:close()
 *
 * What it is made of is what the map's tile server is made of (`tiles.c`):
 * the Network Kit's C client, the TLS core over it, and `httpc` streaming
 * into a sink - here, the region, which fills and is emptied by whoever
 * holds it, so the reading stops while it is full and nothing is lost. The
 * caller writes the region out (`fs.write_from_at`) and checks it
 * (`crypto.sha256_begin`), and the bytes go from the connection to the disk
 * without the interpreter seeing one.
 *
 * A name is looked up by the caller (`net.resolve`), and a redirect is the
 * caller's to follow: it reads `Location` and starts another download where
 * that points. Nothing here waits but `wait`, which is asked for.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kosmos.h"
#include "netclient.h"
#include "httpc.h"
#include "kits/tls/tls_core.h"

#include "lua.h"
#include "lauxlib.h"

#define DL_MT "kosmos.download"

/* The parts, in pages of their own: the TLS state alone is about 40 KB,
 * and a process's heap is 2 MB. */
struct dl_parts {
    struct net_conn conn;
    struct tls_conn secure;
    struct httpc    http;
};

struct dl {
    struct dl_parts *p;
    long     region;            /* the parts' pages */
    unsigned long pages;
    bool     tls, open, closed;

    uint8_t *at;                /* the caller's region */
    size_t   bytes, filled;
    char     why[96];
};

static size_t sink_room(void *user)
{
    struct dl *d = user;

    return d->bytes - d->filled;
}

static int sink_put(void *user, const uint8_t *b, size_t n)
{
    struct dl *d = user;

    if (n > d->bytes - d->filled) return 0;

    memcpy(d->at + d->filled, b, n);
    d->filled += n;
    return 1;
}

static size_t tls_net_write(void *user, const unsigned char *p, size_t n)
{
    long w = net_client_write(user, p, n);

    return w > 0 ? (size_t)w : 0;
}

static size_t tls_net_read(void *user, unsigned char *buf, size_t max)
{
    long r = net_client_read(user, buf, max);

    return r > 0 ? (size_t)r : 0;
}

static long plain_write(void *user, const void *p, size_t n)
{
    return net_client_write(&((struct dl *)user)->p->conn, p, n);
}

static long plain_read(void *user, void *buf, size_t max)
{
    return net_client_read(&((struct dl *)user)->p->conn, buf, max);
}

/* Sent at once: a GET never fills a record, and the far end waits for it
 * otherwise (`tiles.c`, 8 October). */
static long secure_write(void *user, const void *p, size_t n)
{
    struct dl *d = user;
    size_t took;

    if (tls_core_state(&d->p->secure, NULL, NULL, NULL) == TLS_CLOSED) return -1;

    took = tls_core_write(&d->p->secure, p, n);

    if (took > 0) tls_core_flush(&d->p->secure);

    return (long)took;
}

static long secure_read(void *user, void *buf, size_t max)
{
    struct dl *d = user;
    size_t n = tls_core_read(&d->p->secure, buf, max);

    if (n == 0 && (tls_core_state(&d->p->secure, NULL, NULL, NULL) == TLS_CLOSED
                   || net_client_over(&d->p->conn))) {
        return -1;
    }

    return (long)n;
}

static struct dl *check_dl(lua_State *L)
{
    struct dl *d = luaL_checkudata(L, 1, DL_MT);

    if (d->p == NULL) luaL_error(L, "the download is closed");

    return d;
}

static void dl_end(struct dl *d)
{
    if (d->p == NULL) return;

    if (d->tls) {
        if (!d->closed) tls_core_close(&d->p->secure);
        tls_core_free(&d->p->secure);
    }

    httpc_free(&d->p->http);
    net_client_close(&d->p->conn);
    (void)kosmos_share_unmap((unsigned long)(uintptr_t)d->p, d->pages);
    (void)kosmos_cap_drop(d->region);
    d->p = NULL;
}

static lua_Integer int_field(lua_State *L, int t, const char *name, lua_Integer otherwise)
{
    lua_Integer v;

    lua_getfield(L, t, name);
    v = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : otherwise;
    lua_pop(L, 1);
    return v;
}

/* `net.download(cap, spec)` */
int kosmos_net_download(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    const char *host, *path, *address;
    size_t alen = 0;
    lua_Integer port, from, at, bytes;
    const unsigned char *ders[TLS_EXTRA_MAX];
    size_t lens[TLS_EXTRA_MAX];
    int count = 0, insecure;
    struct dl *d;
    long at_parts;
    struct httpc_sink sink;
    struct httpc_io io;

    luaL_checktype(L, 2, LUA_TTABLE);

    lua_getfield(L, 2, "host");
    host = luaL_checkstring(L, -1);
    lua_getfield(L, 2, "path");
    path = luaL_checkstring(L, -1);
    lua_getfield(L, 2, "address");
    address = luaL_checklstring(L, -1, &alen);
    luaL_argcheck(L, alen == 4, 2, "an address is four bytes");

    lua_getfield(L, 2, "tls");
    bool tls = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "insecure");
    insecure = lua_toboolean(L, -1);
    lua_pop(L, 1);

    port = int_field(L, 2, "port", tls ? 443 : 80);
    from = int_field(L, 2, "from", 0);
    at = int_field(L, 2, "at", 0);
    bytes = int_field(L, 2, "bytes", 0);
    luaL_argcheck(L, at != 0 && bytes > 0 && from >= 0 && port > 0 && port < 65536, 2,
                  "a region's `at` and `bytes`, a `from` of nought or more, and a port");

    lua_getfield(L, 2, "anchors");
    if (lua_istable(L, -1)) {
        int list = lua_gettop(L);

        for (lua_Integer i = 1; i <= (lua_Integer)luaL_len(L, list) && count < TLS_EXTRA_MAX; i++) {
            lua_rawgeti(L, list, i);
            ders[count] = (const unsigned char *)luaL_checklstring(L, -1, &lens[count]);
            count++;                    /* the strings stay on the stack until opened */
        }
    }

    d = lua_newuserdatauv(L, sizeof(*d), 0);
    memset(d, 0, sizeof(*d));
    luaL_setmetatable(L, DL_MT);

    d->pages = (sizeof(struct dl_parts) + 4095u) / 4096u;

    if ((d->region = kosmos_mem_create(d->pages)) < 0
        || (at_parts = kosmos_mem_map(d->region)) < 0) {
        if (d->region >= 0) (void)kosmos_cap_drop(d->region);
        lua_pushnil(L);
        lua_pushliteral(L, "no memory for a download");
        return 2;
    }

    d->p = (struct dl_parts *)(uintptr_t)at_parts;
    memset(d->p, 0, sizeof(*d->p));
    d->p->conn.region = -1;
    d->at = (uint8_t *)(uintptr_t)at;
    d->bytes = (size_t)bytes;
    d->tls = tls;

    net_client_start(cap);

    if (net_client_connect(&d->p->conn, (const uint8_t *)address, (uint16_t)port) != NET_OK) {
        d->tls = false;
        dl_end(d);
        lua_pushnil(L);
        lua_pushliteral(L, "the server would not connect");
        return 2;
    }

    if (tls) {
        struct tls_io t = { &d->p->conn, tls_net_write, tls_net_read };
        const char *why = NULL;

        if (tls_core_open(&d->p->secure, host, ders, lens, count, insecure, t, &why) != 0) {
            d->tls = false;
            dl_end(d);
            lua_pushnil(L);
            lua_pushstring(L, why ? why : "TLS would not begin");
            return 2;
        }

        io = (struct httpc_io){ d, secure_write, secure_read };
    } else {
        io = (struct httpc_io){ d, plain_write, plain_read };
    }

    /* A redirect's or a refusal's words, held: a page at most. */
    httpc_init(&d->p->http, io, host, 4096);
    sink = (struct httpc_sink){ d, sink_room, sink_put };

    if (httpc_get_into(&d->p->http, path, (long long)from, sink) != 0) {
        dl_end(d);
        lua_pushnil(L);
        lua_pushliteral(L, "the address is too long");
        return 2;
    }

    d->open = true;
    return 1;
}

static int l_step(lua_State *L)
{
    struct dl *d = check_dl(L);
    int r = httpc_step(&d->p->http);

    switch (r) {
    case HTTPC_SENDING: lua_pushliteral(L, "sending"); return 1;
    case HTTPC_HEAD:    lua_pushliteral(L, "head"); return 1;
    case HTTPC_BODY:    lua_pushliteral(L, "body"); return 1;
    case HTTPC_DONE:    lua_pushliteral(L, "done"); return 1;
    default: {
        const char *why = d->p->http.why, *tls_why = NULL;

        /* A TLS failure says itself better than "closed": a certificate
         * refused, a handshake that went wrong. */
        if (d->tls && tls_core_state(&d->p->secure, &tls_why, NULL, NULL) == TLS_CLOSED
            && tls_why != NULL) {
            why = tls_why;
        }

        lua_pushliteral(L, "failed");
        lua_pushstring(L, why ? why : "the download failed");
        return 2;
    }
    }
}

static int l_ready(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)check_dl(L)->filled);
    return 1;
}

static int l_taken(lua_State *L)
{
    check_dl(L)->filled = 0;
    return 0;
}

static int l_status(lua_State *L)
{
    lua_pushinteger(L, check_dl(L)->p->http.status);
    return 1;
}

static int l_length(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)check_dl(L)->p->http.length);
    return 1;
}

static int l_got(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)check_dl(L)->p->http.got);
    return 1;
}

/* A header's value, or nil; a held body (a redirect's, a refusal's) too. */
static int l_header(lua_State *L)
{
    struct dl *d = check_dl(L);
    size_t len = 0;
    const char *v = httpc_header(&d->p->http, luaL_checkstring(L, 2), &len);

    if (v == NULL) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, v, len);
    }

    return 1;
}

static int l_body(lua_State *L)
{
    struct dl *d = check_dl(L);

    lua_pushlstring(L, d->p->http.body ? (const char *)d->p->http.body : "", d->p->http.body_len);
    return 1;
}

static int l_trusted(lua_State *L)
{
    struct dl *d = check_dl(L);
    const char *why = NULL;
    int t;

    if (!d->tls) {
        lua_pushnil(L);
        return 1;
    }

    t = tls_core_trusted(&d->p->secure, &why);

    if (t < 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushboolean(L, t);
    lua_pushstring(L, why ? why : "");
    return 2;
}

/* Until the connection has bytes or is over, or `ticks` pass (0: for
 * ever) - asked of the stack, which parks this caller and goes on. */
static int l_wait(lua_State *L)
{
    struct dl *d = check_dl(L);
    struct net_poll_entry e;

    memset(&e, 0, sizeof(e));
    e.handle = d->p->conn.handle;
    e.want = NET_WANT_READ;
    (void)net_client_poll(&e, 1, (uint32_t)luaL_optinteger(L, 2, 0));
    return 0;
}

static int l_close(lua_State *L)
{
    struct dl *d = luaL_checkudata(L, 1, DL_MT);

    dl_end(d);
    return 0;
}

void kosmos_net_download_methods(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "step",    l_step },
        { "ready",   l_ready },
        { "taken",   l_taken },
        { "status",  l_status },
        { "length",  l_length },
        { "got",     l_got },
        { "header",  l_header },
        { "body",    l_body },
        { "trusted", l_trusted },
        { "wait",    l_wait },
        { "close",   l_close },
        { "__gc",    l_close },
        { NULL, NULL }
    };

    luaL_newmetatable(L, DL_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pop(L, 1);
}
