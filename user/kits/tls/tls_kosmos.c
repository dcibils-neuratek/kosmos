/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The TLS Kit: `use("/Kosmos/Kits/tls")`, a TCP connection made secure.
 *
 * **HTTPS, step 2** (`roadmap.md`, the browser: TLS) - Diego, 30 September:
 * "Go for it". The protocol is BearSSL's and everything decided about it -
 * whom to trust, the clock, the randomness, Open anyway, the sessions - is
 * `tls_core.c`'s, which a server in C uses as well (`docs/maps.md` M6a).
 * This file is the Lua door: a connection's bytes moved through its own
 * `read` and `write` methods.
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
 * for plain TCP.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "tls_core.h"

#define TLS_MT "kosmos.tls"

struct tls {
    struct tls_conn core;
    lua_State      *L;          /* the state of the call under way */
    int             conn;       /* the connection, in the registry */
    int             open;
};

/* `conn:write(bytes)`: how many it took. */
static size_t lua_write(void *user, const unsigned char *p, size_t n)
{
    struct tls *t = user;
    lua_State *L = t->L;
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

/* `conn:read()`, into `buf`: how many came. */
static size_t lua_read(void *user, unsigned char *buf, size_t max)
{
    struct tls *t = user;
    lua_State *L = t->L;
    size_t n = 0;
    const char *s;

    lua_rawgeti(L, LUA_REGISTRYINDEX, t->conn);
    lua_getfield(L, -1, "read");
    lua_pushvalue(L, -2);
    lua_call(L, 1, 1);

    s = lua_tolstring(L, -1, &n);

    if (s == NULL) {
        n = 0;
    } else {
        if (n > max) {
            n = max;            /* a ring holds no more than this */
        }

        memcpy(buf, s, n);
    }

    lua_pop(L, 2);
    return n;
}

static struct tls *check_tls(lua_State *L)
{
    struct tls *t = luaL_checkudata(L, 1, TLS_MT);

    if (!t->open) {
        luaL_error(L, "this TLS connection was not opened");
    }

    t->L = L;
    return t;
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
    const unsigned char *ders[TLS_EXTRA_MAX];
    size_t lens[TLS_EXTRA_MAX];
    int count = 0, insecure = 0;
    struct tls *t;
    const char *why = NULL;
    struct tls_io io;
    int made;

    luaL_checkany(L, 1);

    t = lua_newuserdatauv(L, sizeof(*t), 1);
    memset(t, 0, sizeof(*t));
    luaL_setmetatable(L, TLS_MT);
    made = lua_gettop(L);

    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "anchors");

        if (lua_istable(L, -1)) {
            int list = lua_gettop(L);       /* the strings go above it */
            lua_Integer i;

            for (i = 1; i <= (lua_Integer)luaL_len(L, list); i++) {
                size_t len;
                const char *der;

                if (count == TLS_EXTRA_MAX) {
                    return luaL_error(L, "tls.client: at most %d anchors", TLS_EXTRA_MAX);
                }

                lua_rawgeti(L, list, i);
                der = luaL_checklstring(L, -1, &len);
                ders[count] = (const unsigned char *)der;
                lens[count] = len;
                count++;

                /* The strings stay on the stack until the handshake has
                 * made anchors of them. */
            }
        }

        lua_getfield(L, 3, "insecure");
        insecure = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    lua_pushvalue(L, 1);
    t->conn = luaL_ref(L, LUA_REGISTRYINDEX);
    t->L = L;

    io.user = t;
    io.write = lua_write;
    io.read = lua_read;

    if (tls_core_open(&t->core, name, ders, lens, count, insecure, io, &why) != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, t->conn);
        t->conn = 0;
        return luaL_error(L, "tls.client: %s", why);
    }

    t->open = 1;

    /* What was made, under the anchors' strings: pushed again to return. */
    lua_pushvalue(L, made);
    return 1;
}

/* `t:write(bytes)` - how many the engine took; 0 until the handshake is done. */
static int l_write(lua_State *L)
{
    struct tls *t = check_tls(L);
    size_t len;
    const char *s = luaL_checklstring(L, 2, &len);

    lua_pushinteger(L, (lua_Integer)tls_core_write(&t->core, s, len));
    return 1;
}

/* `t:flush()` - what was written, sent now rather than when a record fills. */
static int l_flush(lua_State *L)
{
    struct tls *t = check_tls(L);

    tls_core_flush(&t->core);
    return 0;
}

/* `t:read()` - the plaintext that has arrived, or nil. */
static int l_read(lua_State *L)
{
    struct tls *t = check_tls(L);
    size_t len;
    const unsigned char *buf = tls_core_peek(&t->core, &len);

    if (buf == NULL) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, (const char *)buf, len);
    tls_core_take(&t->core, len);
    return 1;
}

/* `t:state()` - "handshake", "open" or "closed", and when closed, why, the
 * error's number, and whether it was the certificate. */
static int l_state(lua_State *L)
{
    struct tls *t = check_tls(L);
    const char *why = NULL;
    int err = 0, certificate = 0;
    int state = tls_core_state(&t->core, &why, &err, &certificate);

    if (state == TLS_CLOSED) {
        lua_pushstring(L, "closed");
        lua_pushstring(L, why);
        lua_pushinteger(L, err);
        lua_pushboolean(L, certificate);
        return 4;
    }

    lua_pushstring(L, state == TLS_OPEN ? "open" : "handshake");
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
    const char *why = NULL;
    int trusted = tls_core_trusted(&t->core, &why);

    if (trusted < 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushboolean(L, trusted);

    if (!trusted) {
        lua_pushstring(L, why);
        return 2;
    }

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
    int resumed = tls_core_resumed(&t->core);

    if (resumed < 0) {
        lua_pushnil(L);
    } else {
        lua_pushboolean(L, resumed);
    }

    return 1;
}

/* `t:close()` - a close_notify, sent. The connection is the caller's. */
static int l_close(lua_State *L)
{
    struct tls *t = check_tls(L);

    tls_core_close(&t->core);
    return 0;
}

static int l_gc(lua_State *L)
{
    struct tls *t = luaL_checkudata(L, 1, TLS_MT);

    if (t->open) {
        tls_core_free(&t->core);
        t->open = 0;
    }

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
    lua_pushinteger(L, (lua_Integer)tls_core_anchors());
    lua_setfield(L, -2, "anchors");
}
