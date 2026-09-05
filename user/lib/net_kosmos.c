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

void kosmos_net_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "info",      l_info },
        { "configure", l_configure },
        { "ping",      l_ping },
        { NULL, NULL }
    };

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
