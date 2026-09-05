/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The network kit: `use("/kits/network")`.
 *
 * `/net` speaks a declared shape - `netproto.h` - and this is the side that
 * builds it. One place that knows the layout, so a program says
 * `net.ping(where, 1)` and never writes a byte offset.
 *
 * **A kit rather than a global**, so the rule the rest of the system runs on
 * still holds: this comes through the namespace, and a program that was not
 * given `use` has no kits. It is also why the capability is a *parameter* to
 * every call here rather than something this file finds for itself - what
 * you were not handed, you cannot reach, and a kit that resolved `/net`
 * itself would be a back door around whoever decided not to mount it.
 *
 * **In C for the reason the console kit is**: a struct on the wire has one
 * layout, and two implementations of it in two languages is two chances to
 * disagree about padding. Not for speed - a ping happens when somebody
 * types.
 *
 * The addresses cross into Lua as strings of four bytes rather than as
 * `"10.0.2.15"`, and back the same way. Text is a *presentation* and
 * `ping.lua` is where it belongs; a kit that parsed dotted quads would be
 * deciding how somebody else writes an address, which is the same argument
 * `hal_pointer_poll` makes about device units.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "netproto.h"
#include "tcpring.h"

#include "lua.h"
#include "lauxlib.h"

static void set_int(lua_State *L, const char *name, lua_Integer v)
{
    lua_pushinteger(L, v);
    lua_setfield(L, -2, name);
}

/* An address out of Lua: four bytes, exactly. Anything else is a caller
 * that built one wrong, and saying so beats sending to 0.0.0.0. */
static void take_addr(lua_State *L, int at, struct net_addr *out)
{
    size_t len = 0;
    const char *s = lua_isnoneornil(L, at) ? NULL
                                           : luaL_checklstring(L, at, &len);

    memset(out, 0, sizeof(*out));

    if (s == NULL) {
        return;
    }

    if (len != 4) {
        luaL_error(L, "an address is four bytes, not %d", (int)len);
        return;
    }

    memcpy(out->byte, s, 4);
}

static void push_addr(lua_State *L, const struct net_addr *a)
{
    lua_pushlstring(L, (const char *)a->byte, 4);
}

/*
 * One exchange with the stack, in C.
 *
 * The reply comes back into the caller's table rather than a fresh one, the
 * same trick `con.wait` uses and for the same reason: `frames` found that a
 * request going out through `sys.call_raw` costs a Lua string each way plus
 * the tables to hold it, and a program that pings once a second does not
 * care - but a program that reads a stream would. The habit is worth having
 * before there is a stream.
 */
static long exchange(lua_State *L, long cap, const struct net_request *req,
                     struct net_reply *out)
{
    struct message msg, rep;
    long status;

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(*req);
    memcpy(msg.data, req, sizeof(*req));

    status = kosmos_call(cap, &msg, &rep);

    if (status != 0 || rep.length < sizeof(*out)) {
        return -1;
    }

    memcpy(out, rep.data, sizeof(*out));

    (void)L;
    return 0;
}

/*
 * `net.info(cap)` - what this stack is.
 *
 * A table, or nil and a reason. `has_card` false is not an error: a machine
 * with no network is a machine, and a program that asks should be told
 * rather than raised at.
 */
static int l_info(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct net_request req;
    struct net_reply rep;

    memset(&req, 0, sizeof(req));
    req.op = NET_OP_INFO;

    if (exchange(L, cap, &req, &rep) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "the network stack did not answer");
        return 2;
    }

    lua_createtable(L, 0, 6);

    lua_pushboolean(L, rep.has_card != 0);
    lua_setfield(L, -2, "card");

    lua_pushlstring(L, (const char *)rep.mac, sizeof(rep.mac));
    lua_setfield(L, -2, "mac");

    set_int(L, "mtu", (lua_Integer)rep.mtu);

    push_addr(L, &rep.address);
    lua_setfield(L, -2, "address");
    push_addr(L, &rep.netmask);
    lua_setfield(L, -2, "netmask");
    push_addr(L, &rep.gateway);
    lua_setfield(L, -2, "gateway");

    return 1;
}

/* `net.configure(cap, address, netmask, gateway)` */
static int l_configure(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct net_request req;
    struct net_reply rep;

    memset(&req, 0, sizeof(req));
    req.op = NET_OP_CONFIG;

    take_addr(L, 2, &req.address);
    take_addr(L, 3, &req.netmask);
    take_addr(L, 4, &req.gateway);

    if (exchange(L, cap, &req, &rep) != 0 || rep.status != NET_OK) {
        lua_pushnil(L);
        lua_pushinteger(L, (lua_Integer)rep.status);
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

/*
 * `net.ping(cap, address, seq [, payload])` - one echo, and the answer.
 *
 * **This blocks until the reply comes back or the stack gives up on it**,
 * which is what a ping is. The stack does not block: it parks this caller
 * and goes on serving, so one program waiting on a host that is not there
 * does not stop another from reading a file. That is the whole reason the
 * stack is a server rather than this kit doing the work.
 *
 * Returns a table with the round trip in *counter ticks*, undecoded. The
 * caller divides by `counter_hz` from `/dev/cpu`, because that is 62.5 MHz
 * under QEMU's TCG and 24 MHz when the same machine runs under `hvf`, and a
 * kit that converted here would bake one of them in.
 */
static int l_ping(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct net_request req;
    struct net_reply rep;
    size_t len = 0;
    const char *payload = luaL_optlstring(L, 4, "", &len);

    memset(&req, 0, sizeof(req));
    req.op  = NET_OP_PING;
    req.seq = (uint32_t)luaL_checkinteger(L, 3);

    take_addr(L, 2, &req.to);

    if (len > NET_PAYLOAD_MAX) {
        len = NET_PAYLOAD_MAX;
    }

    req.length = (uint32_t)len;
    memcpy(req.payload, payload, len);

    if (exchange(L, cap, &req, &rep) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "the network stack did not answer");
        return 2;
    }

    if (rep.status != NET_OK) {
        lua_pushnil(L);
        lua_pushinteger(L, (lua_Integer)rep.status);
        return 2;
    }

    lua_createtable(L, 0, 5);

    set_int(L, "seq",   (lua_Integer)rep.seq);
    set_int(L, "ttl",   (lua_Integer)rep.ttl);
    set_int(L, "ticks", (lua_Integer)rep.ticks);
    set_int(L, "bytes", (lua_Integer)rep.length);

    push_addr(L, &rep.from);
    lua_setfield(L, -2, "from");

    return 1;
}

/*------------------------------------------------------------------------
 * Connections.
 *
 * The bytes are in a region both sides hold, so what these do is arithmetic
 * on two indices and a `memcpy`. Nothing here builds a message per byte, and
 * `tcpring.h` says why that matters before the first one moved.
 *----------------------------------------------------------------------*/

/* The region a connection's rings live in, mapped in this process. Held in
 * a Lua userdata so it goes when the handle does. */
struct ring_handle {
    struct tcp_ring *ring;
    long             cap;
    uint32_t         handle;
    long             net_cap;
};

static struct ring_handle *checkring(lua_State *L, int at)
{
    return (struct ring_handle *)luaL_checkudata(L, at, "kosmos.tcp");
}

/*
 * `net.connect(cap, address, port)` - open one, and get a handle.
 *
 * **This blocks until the far end answers or the stack gives up.** The stack
 * does not: it parks this caller in `call` and goes on serving, the same
 * arrangement `ping` uses. What comes back is a capability to the region
 * holding the two rings, which is mapped here and never travels again.
 */
static int l_connect(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct net_request req;
    struct net_reply rep;
    struct message msg, out;
    struct ring_handle *h;
    long at;
    long region;

    memset(&req, 0, sizeof(req));
    req.op   = NET_OP_CONNECT;
    req.port = (uint32_t)luaL_checkinteger(L, 3);

    take_addr(L, 2, &req.to);

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(req);
    memcpy(msg.data, &req, sizeof(req));

    if (kosmos_call(cap, &msg, &out) != 0 || out.length < sizeof(rep)) {
        lua_pushnil(L);
        lua_pushliteral(L, "the network stack did not answer");
        return 2;
    }

    memcpy(&rep, out.data, sizeof(rep));

    if (rep.status != NET_OK) {
        lua_pushnil(L);
        lua_pushinteger(L, (lua_Integer)rep.status);
        return 2;
    }

    if (out.cap_plus_one == 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "the stack opened it but sent no rings");
        return 2;
    }

    region = (long)out.cap_plus_one - 1;
    at = kosmos_mem_map(region);

    if (at < 0) {
        (void)kosmos_cap_drop(region);
        lua_pushnil(L);
        lua_pushliteral(L, "the rings could not be mapped");
        return 2;
    }

    h = (struct ring_handle *)lua_newuserdatauv(L, sizeof(*h), 0);
    h->ring    = (struct tcp_ring *)(uintptr_t)at;
    h->cap     = region;
    h->handle  = rep.handle;
    h->net_cap = cap;

    luaL_setmetatable(L, "kosmos.tcp");

    return 1;
}

/*
 * `conn:write(text)` - into the ring, then tell the stack.
 *
 * Returns how many bytes were taken, which may be fewer than were offered:
 * the ring is finite and a caller that is faster than the network has to
 * find that out. Silently dropping the rest would be a connection that
 * loses data with no error anywhere.
 */
static int l_write(lua_State *L)
{
    struct ring_handle *h = checkring(L, 1);
    size_t len = 0;
    const char *text = luaL_checklstring(L, 2, &len);
    struct net_request req;
    struct message msg, out;
    uint32_t write = h->ring->out_write;
    uint32_t space = tcp_ring_space(h->ring->bytes, write,
                                    tcp_ring_acquire(&h->ring->out_read));
    size_t take = (len > space) ? space : len;
    size_t i;

    for (i = 0; i < take; i++) {
        tcp_ring_out(h->ring)[(write + i) % h->ring->bytes]
            = (uint8_t)text[i];
    }

    tcp_ring_publish(&h->ring->out_write, write + (uint32_t)take);

    memset(&req, 0, sizeof(req));
    req.op     = NET_OP_PUSH;
    req.handle = h->handle;

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(req);
    memcpy(msg.data, &req, sizeof(req));

    (void)kosmos_call(h->net_cap, &msg, &out);

    lua_pushinteger(L, (lua_Integer)take);
    return 1;
}

/*
 * `conn:read()` - whatever has arrived, or nil.
 *
 * Nil means nothing is waiting, which is not an error and not the end: a
 * closed connection is `conn:closed()`, and the bytes already in the ring
 * are readable after it. A far end that sent a line and hung up sent that
 * line.
 */
static int l_read(lua_State *L)
{
    struct ring_handle *h = checkring(L, 1);
    uint32_t read = h->ring->in_read;
    uint32_t ready = tcp_ring_ready(tcp_ring_acquire(&h->ring->in_write),
                                    read);
    luaL_Buffer b;
    uint32_t i;

    if (ready == 0) {
        lua_pushnil(L);
        return 1;
    }

    luaL_buffinit(L, &b);

    for (i = 0; i < ready; i++) {
        luaL_addchar(&b, (char)tcp_ring_in(h->ring)[(read + i)
                                                    % h->ring->bytes]);
    }

    tcp_ring_publish(&h->ring->in_read, read + ready);
    luaL_pushresult(&b);

    return 1;
}

/*
 * `conn:wait(ticks)` - block until something arrives or it closes.
 *
 * The alternative is a loop calling `read`, which is a process that never
 * blocks and a core that is gone - the same measurement that put a deadline
 * in every other server's receive.
 */
static int l_wait(lua_State *L)
{
    struct ring_handle *h = checkring(L, 1);
    struct net_request req;
    struct net_reply rep;
    struct message msg, out;

    memset(&req, 0, sizeof(req));
    req.op     = NET_OP_WAIT;
    req.handle = h->handle;
    req.ticks  = (uint32_t)luaL_optinteger(L, 2, 0);

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(req);
    memcpy(msg.data, &req, sizeof(req));

    if (kosmos_call(h->net_cap, &msg, &out) != 0 || out.length < sizeof(rep)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    memcpy(&rep, out.data, sizeof(rep));
    lua_pushboolean(L, rep.status == NET_OK);

    return 1;
}

static int l_closed(lua_State *L)
{
    struct ring_handle *h = checkring(L, 1);

    lua_pushboolean(L, h->ring->closed != 0);
    return 1;
}

static int l_close(lua_State *L)
{
    struct ring_handle *h = checkring(L, 1);
    struct net_request req;
    struct message msg, out;

    memset(&req, 0, sizeof(req));
    req.op     = NET_OP_CLOSE;
    req.handle = h->handle;

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(req);
    memcpy(msg.data, &req, sizeof(req));

    (void)kosmos_call(h->net_cap, &msg, &out);

    lua_pushboolean(L, 1);
    return 1;
}

/*
 * `net.listen(cap, port)` - answer on a port.
 *
 * Returns a plain handle rather than a userdata, because a listener has no
 * ring: there is nothing to map and nothing to free but the slot. What comes
 * out of `accept` is the connection, and that is a userdata like any other.
 */
static int l_listen(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct net_request req;
    struct net_reply rep;

    memset(&req, 0, sizeof(req));
    req.op   = NET_OP_LISTEN;
    req.port = (uint32_t)luaL_checkinteger(L, 2);

    if (exchange(L, cap, &req, &rep) != 0 || rep.status != NET_OK) {
        lua_pushnil(L);
        lua_pushinteger(L, (lua_Integer)rep.status);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)rep.handle);
    return 1;
}

/*
 * `net.accept(cap, listener)` - park until somebody connects.
 *
 * **This blocks and the stack does not.** The caller sits inside `call`
 * while the stack goes on answering everything else, which is the same
 * arrangement `connect` and `ping` use and the reason a server written on
 * this does not need threads to stay responsive to its own other requests.
 */
static int l_accept(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct net_request req;
    struct net_reply rep;
    struct message msg, out;
    struct ring_handle *h;
    long region, at;

    memset(&req, 0, sizeof(req));
    req.op     = NET_OP_ACCEPT;
    req.handle = (uint32_t)luaL_checkinteger(L, 2);

    /* How long to wait for somebody, in scheduler ticks; nothing means for
     * ever, which is what a program with only one thing to do wants. An
     * event loop passes a deadline - see NET_OP_ACCEPT in `net.c` for the
     * race that makes it necessary. */
    req.ticks = (uint32_t)luaL_optinteger(L, 3, 0);

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(req);
    memcpy(msg.data, &req, sizeof(req));

    if (kosmos_call(cap, &msg, &out) != 0 || out.length < sizeof(rep)) {
        lua_pushnil(L);
        lua_pushliteral(L, "the network stack did not answer");
        return 2;
    }

    memcpy(&rep, out.data, sizeof(rep));

    if (rep.status != NET_OK || out.cap_plus_one == 0) {
        lua_pushnil(L);
        lua_pushinteger(L, (lua_Integer)rep.status);
        return 2;
    }

    region = (long)out.cap_plus_one - 1;
    at = kosmos_mem_map(region);

    if (at < 0) {
        (void)kosmos_cap_drop(region);
        lua_pushnil(L);
        lua_pushliteral(L, "the rings could not be mapped");
        return 2;
    }

    h = (struct ring_handle *)lua_newuserdatauv(L, sizeof(*h), 0);
    h->ring    = (struct tcp_ring *)(uintptr_t)at;
    h->cap     = region;
    h->handle  = rep.handle;
    h->net_cap = cap;

    luaL_setmetatable(L, "kosmos.tcp");

    /* And who it is from, because a server that cannot say who connected
     * cannot keep a log worth reading. */
    push_addr(L, &rep.from);

    return 2;
}

/*
 * `net.poll(cap, handles, listener, ticks)` - wait on several at once.
 *
 * `handles` is a table of connections; the mask is built here so a caller
 * never sees one. Returns a table of the ready connections and whether the
 * listener has somebody waiting.
 *
 * **This is the `select` the system has wanted six times** and the reason a
 * server can serve more than one request at a time here: with it, `httpd`
 * runs a coroutine per connection and resumes whichever is ready. Without
 * it, everything that needed to watch two things settled for a timer.
 */
/* The connections a table names, as a bitmask. */
static uint32_t mask_of(lua_State *L, int index)
{
    uint32_t mask = 0;
    lua_Integer n, i;

    if (lua_isnoneornil(L, index)) {
        return 0;
    }

    luaL_checktype(L, index, LUA_TTABLE);
    n = (lua_Integer)lua_rawlen(L, index);

    for (i = 1; i <= n; i++) {
        struct ring_handle *h;

        lua_rawgeti(L, index, i);
        h = (struct ring_handle *)luaL_testudata(L, -1, "kosmos.tcp");

        if (h != NULL && h->handle < NET_CONN_MAX) {
            mask |= 1u << h->handle;
        }

        lua_pop(L, 1);
    }

    return mask;
}

/*
 * Move the ready ones out of the table at `index` and into the result on
 * top of the stack, which is what lets a caller use what comes back
 * directly rather than looking numbers up in a table of its own.
 *
 * `taken` is carried between the two calls so a connection named in both
 * lists appears once.
 */
static int collect(lua_State *L, int index, uint32_t ready,
                   uint32_t *taken, int at)
{
    lua_Integer n, i;

    if (lua_isnoneornil(L, index)) {
        return at;
    }

    n = (lua_Integer)lua_rawlen(L, index);

    for (i = 1; i <= n; i++) {
        struct ring_handle *h;
        uint32_t bit;

        lua_rawgeti(L, index, i);
        h = (struct ring_handle *)luaL_testudata(L, -1, "kosmos.tcp");
        bit = (h != NULL && h->handle < NET_CONN_MAX) ? 1u << h->handle : 0u;

        if (bit != 0 && (ready & bit) != 0 && (*taken & bit) == 0) {
            *taken |= bit;
            lua_rawseti(L, -2, at++);   /* moves it into the result */
        } else {
            lua_pop(L, 1);
        }
    }

    return at;
}

/*
 * poll(cap, reading, writing, listener, ticks) -> ready, arrived
 *
 * Two lists because they are two questions - see `NET_OP_POLL` in
 * `netproto.h`. What comes back is one list of whichever connections are
 * ready, in the order they were asked about, and whether somebody is
 * waiting on the listener.
 */
static int l_poll(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct net_request req;
    struct net_reply rep;

    memset(&req, 0, sizeof(req));

    req.op    = NET_OP_POLL;
    req.ticks = (uint32_t)luaL_optinteger(L, 5, 0);

    /* The listener, plus one, so that absent is zero. */
    req.port = lua_isnoneornil(L, 4)
               ? 0u : (uint32_t)luaL_checkinteger(L, 4) + 1u;

    req.handle  = mask_of(L, 2);
    req.writing = mask_of(L, 3);

    if (exchange(L, cap, &req, &rep) != 0 || rep.status != NET_OK) {
        lua_pushnil(L);
        lua_pushinteger(L, (lua_Integer)rep.status);
        return 2;
    }

    lua_newtable(L);

    {
        uint32_t taken = 0;
        int at = collect(L, 2, rep.ready, &taken, 1);

        (void)collect(L, 3, rep.ready, &taken, at);
    }

    lua_pushboolean(L, rep.arrived != 0);
    return 2;
}


void kosmos_net_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "info",      l_info },
        { "configure", l_configure },
        { "ping",      l_ping },
        { "connect",   l_connect },
        { "listen",    l_listen },
        { "accept",    l_accept },
        { "poll",      l_poll },
        { NULL, NULL }
    };

    static const luaL_Reg conn[] = {
        { "write",  l_write },
        { "read",   l_read },
        { "wait",   l_wait },
        { "closed", l_closed },
        { "close",  l_close },
        { NULL, NULL }
    };

    /* The connection type. A userdata with methods rather than a handle
     * number, so a connection cannot be named by a program that was not
     * given one - the same reason everything else here is a capability. */
    luaL_newmetatable(L, "kosmos.tcp");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, conn, 0);
    lua_pop(L, 1);

    luaL_newlib(L, api);

    /* The reasons a call can fail, by name, so a caller writes
     * `net.ERR_UNREACHABLE` rather than remembering that it is 4. */
    set_int(L, "OK",              NET_OK);
    set_int(L, "ERR_BAD_OP",      NET_ERR_BAD_OP);
    set_int(L, "ERR_NO_CARD",     NET_ERR_NO_CARD);
    set_int(L, "ERR_NO_ROUTE",    NET_ERR_NO_ROUTE);
    set_int(L, "ERR_UNREACHABLE", NET_ERR_UNREACHABLE);
    set_int(L, "ERR_FULL",        NET_ERR_FULL);
    set_int(L, "ERR_BAD_ADDRESS", NET_ERR_BAD_ADDRESS);

    set_int(L, "PAYLOAD_MAX", NET_PAYLOAD_MAX);
}
