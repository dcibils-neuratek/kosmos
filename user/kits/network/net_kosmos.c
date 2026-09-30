/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The network kit: `use("/Kosmos/Kits/network")`.
 *
 * `/Network` speaks a declared shape - `netproto.h` - and this is the side that
 * builds it. One place that knows the layout, so a program says
 * `net.ping(where, 1)` and never writes a byte offset.
 *
 * **A kit rather than a global**, so the rule the rest of the system runs on
 * still holds: this comes through the namespace, and a program that was not
 * given `use` has no kits. It is also why the capability is a *parameter* to
 * every call here rather than something this file finds for itself - what
 * you were not handed, you cannot reach, and a kit that resolved `/Network`
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
 * A failure, as its number and as a sentence.
 *
 * The number is what a program compares - `net.ERR_NO_ROUTE` - and the
 * sentence what it shows a person. Every program that showed one kept its
 * own table of them, and three of those tables - `fetch`, `telnet` and the
 * browser's - called 4 "no route", which is 3; so the browser said "could
 * not connect: 3" for the one failure everybody meets, a page asked for
 * before the machine had its address. Said once, here, beside the numbers
 * `netproto.h` gives them.
 */
static const char *sentence(uint32_t status)
{
    switch (status) {
    case NET_ERR_BAD_OP:      return "the network stack did not understand the request";
    case NET_ERR_NO_CARD:     return "this machine has no network";
    case NET_ERR_NO_ROUTE:    return "no route to it: this machine has no address, "
                                     "or nothing here knows the way";
    case NET_ERR_UNREACHABLE: return "nobody answered for that address";
    case NET_ERR_FULL:        return "too many at once";
    case NET_ERR_BAD_ADDRESS: return "that is not an address";
    case NET_ERR_REFUSED:     return "connection refused";
    case NET_ERR_CLOSED:      return "the connection is over";
    case NET_ERR_TIMEOUT:     return "nobody answered in time";
    case NET_ERR_NO_HANDLE:   return "no such connection";
    case NET_ERR_NO_RESOLVER: return "no DNS server is configured";
    case NET_ERR_NO_NAME:     return "there is no such name";
    default:                  return "the network stack did not answer";
    }
}

/* nil, the number, and the sentence. */
static int failed(lua_State *L, uint32_t status)
{
    lua_pushnil(L);
    lua_pushinteger(L, (lua_Integer)status);
    lua_pushstring(L, sentence(status));
    return 3;
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
        out->status = ~0u;              /* no answer: a number, not garbage */
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
    push_addr(L, &rep.dns);
    lua_setfield(L, -2, "dns");

    /* How the address was come by, as a word, and a lease's length. */
    lua_pushstring(L, rep.addressed_by == NET_ADDRESS_GIVEN  ? "given"
                    : rep.addressed_by == NET_ADDRESS_ASKING ? "asking"
                    : rep.addressed_by == NET_ADDRESS_LEASED ? "dhcp" : "none");
    lua_setfield(L, -2, "addressed_by");
    set_int(L, "lease_seconds", (lua_Integer)rep.lease_seconds);
    push_addr(L, &rep.lease_from);
    lua_setfield(L, -2, "lease_from");

    return 1;
}

/*
 * `net.dhcp(cap)` - ask the network for an address. Answered at once;
 * `net.info` says when there is one.
 */
static int l_dhcp(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct net_request req;
    struct net_reply rep;

    memset(&req, 0, sizeof(req));
    req.op = NET_OP_DHCP;

    if (exchange(L, cap, &req, &rep) != 0 || rep.status != NET_OK) {
        return failed(L, rep.status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* `net.configure(cap, address, netmask, gateway [, dns])` */
/*
 * `net.resolve(cap, name [, ticks])` - a name, as four numbers.
 *
 * Blocks until the resolver answers or the stack gives up, which is what a
 * lookup is. The stack itself does not block: it parks this caller and goes
 * on serving everybody else, the same way `ping` and `accept` do.
 */
static int l_resolve(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    size_t len = 0;
    const char *name = luaL_checklstring(L, 2, &len);
    struct net_request req;
    struct net_reply rep;

    if (len == 0 || len > NET_PAYLOAD_MAX) {
        lua_pushnil(L);
        lua_pushliteral(L, "that is not a name");
        return 2;
    }

    memset(&req, 0, sizeof(req));
    req.op     = NET_OP_RESOLVE;
    req.length = (uint32_t)len;
    req.wait_ticks = (uint32_t)luaL_optinteger(L, 3, 0);
    memcpy(req.payload, name, len);

    if (exchange(L, cap, &req, &rep) != 0 || rep.status != NET_OK) {
        return failed(L, rep.status);
    }

    lua_pushlstring(L, (const char *)rep.address.byte, 4);
    return 1;
}

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
    take_addr(L, 5, &req.dns);

    if (exchange(L, cap, &req, &rep) != 0 || rep.status != NET_OK) {
        return failed(L, rep.status);
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
 * caller divides by `counter_hz` from `/Devices/cpu`, because that is 62.5 MHz
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
        return failed(L, rep.status);
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

/*
 * The region a connection's rings live in, mapped in this process. Held in
 * a Lua userdata so it goes when the handle does - which this comment said
 * for months with nothing to make it true: there was no `__gc`, so every
 * connection a program ever made stayed mapped in it, 36 KB and a
 * capability each. `l_gc` below is that.
 */
struct ring_handle {
    struct tcp_ring *ring;
    long             cap;
    uint64_t         handle;
    long             net_cap;
    int              closed;        /* `close` was asked for */

    /* Where it is in the poll set this call is building, and which call
     * that is, so a connection named for reading and for writing is one
     * entry with both wants. */
    uint32_t         poll_round;
    uint32_t         poll_index;
};

static struct ring_handle *checkring(lua_State *L, int at)
{
    return (struct ring_handle *)luaL_checkudata(L, at, "kosmos.tcp");
}

/*
 * `net.connect(cap, address, port [, at_once])` - open one, and get a handle;
 * with `at_once`, while it is still opening (`NET_CONNECT_AT_ONCE`).
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

    /* `at_once`: the connection back while it is still opening. */
    if (lua_toboolean(L, 4)) {
        req.flags = NET_CONNECT_AT_ONCE;
    }

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
        return failed(L, rep.status);
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
    req.wait_ticks = (uint32_t)luaL_optinteger(L, 2, 0);

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

/* This end is done sending: the FIN, once the ring has gone. */
static void send_close(struct ring_handle *h)
{
    struct net_request req;
    struct message msg, out;

    h->closed = 1;

    memset(&req, 0, sizeof(req));
    req.op     = NET_OP_CLOSE;
    req.handle = h->handle;

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(req);
    memcpy(msg.data, &req, sizeof(req));

    (void)kosmos_call(h->net_cap, &msg, &out);
}

static int l_close(lua_State *L)
{
    struct ring_handle *h = checkring(L, 1);

    if (h->ring != NULL) {
        send_close(h);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/*
 * The connection gone from Lua: closed, if its program never said so, and
 * its ring let go - unmapped first and the capability dropped after, since
 * a region's pages live as long as a capability does and not a mapping.
 * The stack gives its slot back once TCP is finished with it too.
 */
static int l_gc(lua_State *L)
{
    struct ring_handle *h = checkring(L, 1);

    if (h->ring == NULL) {
        return 0;
    }

    if (!h->closed) {
        send_close(h);
    }

    /* The share window's call: `kosmos_unmap` refuses a region's address,
     * and dropping the capability of a region still mapped is how its pages
     * come to be freed under a mapping (`net.c`, `conn_free`). */
    if (kosmos_share_unmap((unsigned long)(uintptr_t)h->ring,
                           (TCP_RING_REGION + 4095u) / 4096u) == 0) {
        (void)kosmos_cap_drop(h->cap);
    }

    h->ring = NULL;
    return 0;
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
        return failed(L, rep.status);
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
    req.handle = (uint64_t)luaL_checkinteger(L, 2);

    /* How long to wait for somebody, in scheduler ticks; nothing means for
     * ever, which is what a program with only one thing to do wants. An
     * event loop passes a deadline - see NET_OP_ACCEPT in `net.c` for the
     * race that makes it necessary. */
    req.wait_ticks = (uint32_t)luaL_optinteger(L, 3, 0);

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
        return failed(L, rep.status);
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
 * `net.poll(cap, reading, writing, listener, ticks)` - wait on several at
 * once, and hear which are ready and whether somebody has arrived.
 *
 * **This is the `select` the system has wanted six times** and the reason a
 * server can serve more than one request at a time here: with it, `httpd`
 * runs a coroutine per connection and resumes whichever is ready. Without
 * it, everything that needed to watch two things settled for a timer.
 *
 * **The set goes in a region, which this process keeps and sends with each
 * call** (`netproto.h`, `NET_OP_POLL`): an entry a connection - its handle
 * and what is wanted of it - and the stack writes back what is so. It was a
 * bitmask in the request, which held a machine to sixteen connections. The
 * region grows when a call needs more room and is otherwise the same one,
 * so a server's loop makes no garbage and no region per pass.
 */
static struct {
    long                   region;
    struct net_poll_entry *set;
    unsigned long          pages;
    uint32_t               capacity;
    uint32_t               round;
} pollset = { -1, NULL, 0, 0, 0 };

/* Room for `want` entries, the region made larger when it has not. */
static bool pollset_room(uint32_t want)
{
    unsigned long pages;
    long region, at;

    if (pollset.set != NULL && want <= pollset.capacity) {
        return true;
    }

    pages = ((unsigned long)want * sizeof(struct net_poll_entry) + 4095u) / 4096u;
    pages = pages < 1 ? 1 : pages;

    if (pages < pollset.pages * 2) {
        pages = pollset.pages * 2;
    }

    region = kosmos_mem_create(pages);

    if (region < 0) {
        return false;
    }

    at = kosmos_mem_map(region);

    if (at < 0) {
        (void)kosmos_cap_drop(region);
        return false;
    }

    if (pollset.set != NULL
        && kosmos_share_unmap((unsigned long)(uintptr_t)pollset.set, pollset.pages) == 0) {
        (void)kosmos_cap_drop(pollset.region);
    }

    pollset.region   = region;
    pollset.set      = (struct net_poll_entry *)(uintptr_t)at;
    pollset.pages    = pages;
    pollset.capacity = (uint32_t)(pages * 4096u / sizeof(struct net_poll_entry));
    return true;
}

/* The connections a list names, into the set: a new entry each, or the
 * want added to the entry the other list made. Returns the count so far. */
static uint32_t pollset_add(lua_State *L, int index, uint32_t want, uint32_t n)
{
    lua_Integer len, i;

    if (lua_isnoneornil(L, index)) {
        return n;
    }

    luaL_checktype(L, index, LUA_TTABLE);
    len = (lua_Integer)lua_rawlen(L, index);

    for (i = 1; i <= len; i++) {
        struct ring_handle *h;

        lua_rawgeti(L, index, i);
        h = (struct ring_handle *)luaL_testudata(L, -1, "kosmos.tcp");
        lua_pop(L, 1);

        if (h == NULL || h->ring == NULL) {
            continue;
        }

        if (h->poll_round == pollset.round) {
            pollset.set[h->poll_index].want |= want;
            continue;
        }

        h->poll_round = pollset.round;
        h->poll_index = n;
        pollset.set[n].handle = h->handle;
        pollset.set[n].want   = want;
        pollset.set[n].got    = 0;
        n++;
    }

    return n;
}

/*
 * The ones of a list that are ready - what was asked of each, or over - into
 * the result on top of the stack, once each however many lists named it.
 */
static int pollset_collect(lua_State *L, int index, uint32_t want, int at)
{
    lua_Integer len, i;

    if (lua_isnoneornil(L, index)) {
        return at;
    }

    len = (lua_Integer)lua_rawlen(L, index);

    for (i = 1; i <= len; i++) {
        struct ring_handle *h;
        struct net_poll_entry *e;

        lua_rawgeti(L, index, i);
        h = (struct ring_handle *)luaL_testudata(L, -1, "kosmos.tcp");

        if (h == NULL || h->ring == NULL || h->poll_round != pollset.round) {
            lua_pop(L, 1);
            continue;
        }

        e = &pollset.set[h->poll_index];

        if ((e->got & (want | NET_GOT_OVER)) != 0 && (e->want & 0x80000000u) == 0) {
            e->want |= 0x80000000u;     /* taken: once in the result */
            lua_rawseti(L, -2, at++);
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
    struct message msg, out;
    uint32_t want = 0, n = 0, listener_at = 0;
    bool listening = !lua_isnoneornil(L, 4);

    if (!lua_isnoneornil(L, 2)) want += (uint32_t)lua_rawlen(L, 2);
    if (!lua_isnoneornil(L, 3)) want += (uint32_t)lua_rawlen(L, 3);
    if (listening) want++;

    if (!pollset_room(want > 0 ? want : 1)) {
        return failed(L, NET_ERR_FULL);
    }

    pollset.round++;
    n = pollset_add(L, 2, NET_WANT_READ, n);
    n = pollset_add(L, 3, NET_WANT_WRITE, n);

    if (listening) {
        listener_at = n;
        pollset.set[n].handle = (uint64_t)luaL_checkinteger(L, 4);
        pollset.set[n].want   = NET_WANT_ACCEPT;
        pollset.set[n].got    = 0;
        n++;
    }

    memset(&req, 0, sizeof(req));
    req.op         = NET_OP_POLL;
    req.wait_ticks = (uint32_t)luaL_optinteger(L, 5, 0);
    req.length     = n;

    /* Nothing to wait on is a wait for the deadline and no more. */
    if (n == 0) {
        req.length = 1;
        pollset.set[0].handle = 0;
        pollset.set[0].want   = 0;
        pollset.set[0].got    = 0;
    }

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(req);
    msg.cap_plus_one = (uint32_t)(pollset.region + 1);
    memcpy(msg.data, &req, sizeof(req));

    if (kosmos_call(cap, &msg, &out) != 0 || out.length < sizeof(rep)) {
        return failed(L, ~0u);
    }

    memcpy(&rep, out.data, sizeof(rep));

    if (rep.status != NET_OK) {
        return failed(L, rep.status);
    }

    lua_newtable(L);

    {
        int at = pollset_collect(L, 2, NET_WANT_READ, 1);

        (void)pollset_collect(L, 3, NET_WANT_WRITE, at);
    }

    lua_pushboolean(L, listening && n > 0
                       && (pollset.set[listener_at].got & NET_WANT_ACCEPT) != 0);
    return 2;
}


void kosmos_net_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "info",      l_info },
        { "configure", l_configure },
        { "dhcp",      l_dhcp },
        { "ping",      l_ping },
        { "connect",   l_connect },
        { "listen",    l_listen },
        { "accept",    l_accept },
        { "poll",      l_poll },
        { "resolve",   l_resolve },
        { NULL, NULL }
    };

    static const luaL_Reg conn[] = {
        { "write",  l_write },
        { "read",   l_read },
        { "wait",   l_wait },
        { "closed", l_closed },
        { "close",  l_close },
        { "__gc",   l_gc },
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
