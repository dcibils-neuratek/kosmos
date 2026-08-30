/*
 * `sys`, from inside a process.
 *
 * The same names the kernel's copy offers, over syscalls instead of direct
 * calls. A Lua program written against one runs against the other, which is
 * the property that will let servers move out of the kernel one at a time
 * rather than all at once.
 *
 * The inspection half is missing, and deliberately. `sys.threads` and
 * `sys.memory` read kernel state, and a process has no business reading it
 * directly: at M5 that is `/proc`, reached through the namespace like any
 * other resource, and reached only by a process that was handed it.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"
#include "serialize.h"

/* The two definitions of struct message are one contract written twice, so
 * the agreement is checked rather than assumed. A silent disagreement would
 * be read as a disagreement about the contents. */
_Static_assert(sizeof(struct message) == 8 + 4 + MSG_BYTES + 4,
               "struct message must match kernel/ipc.h");

static const char *ipc_error(long status)
{
    switch (status) {
    case -1:   return "no such capability";
    case -2:   return "the endpoint was destroyed";
    case -3:   return "that thread is not waiting for a reply";
    case -4:   return "out of endpoints or capability slots";
    case -101: return "a pointer this process may not use";
    default:   return "unknown error";
    }
}

static int fail(lua_State *L, long status)
{
    lua_pushnil(L);
    lua_pushstring(L, ipc_error(status));
    return 2;
}

/*
 * A message carries a Lua value, and this is the only place that knows how.
 *
 * `design.md` §1: the protocol between servers *is* the data model of the
 * userland language. A caller passes a table and a server receives a table;
 * neither writes marshalling, because there is none to write.
 */
static void push_message(lua_State *L, const struct message *m)
{
    int rc = serialize_unpack(L, m);

    if (rc != SERIALIZE_OK) {
        /* Malformed bytes are an error rather than a half-built table. The
         * unpacker leaves the stack as it found it, so raising here is
         * safe. */
        luaL_error(L, "%s", serialize_error(rc));
    }
}

static void take_message(lua_State *L, int index, struct message *m)
{
    int rc;

    m->tag = 0;
    m->length = 0;

    /* A tag, if the value is a table with one. `design.md` §14 wants every
     * message to say what it is; the kernel only insists the field exists,
     * and what goes in it is between the two ends. */
    if (lua_istable(L, index)) {
        lua_getfield(L, index, "tag");
        m->tag = (uint64_t)luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
    }

    rc = serialize_pack(L, index, m);

    if (rc != SERIALIZE_OK) {
        luaL_error(L, "%s", serialize_error(rc));
    }
}

static int l_write(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);

    lua_pushinteger(L, (lua_Integer)kosmos_write(s, len));
    return 1;
}

static int l_exit(lua_State *L)
{
    kosmos_exit((int)luaL_optinteger(L, 1, 0));
    return 0;
}

static int l_yield(lua_State *L)
{
    (void)L;
    kosmos_yield();
    return 0;
}

static int l_endpoint(lua_State *L)
{
    long cap = kosmos_endpoint();

    if (cap < 0) {
        return fail(L, cap);
    }

    lua_pushinteger(L, (lua_Integer)cap);
    return 1;
}

static int l_call(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct message msg;
    struct message reply;
    long status;

    take_message(L, 2, &msg);

    status = kosmos_call(cap, &msg, &reply);
    if (status != 0) {
        return fail(L, status);
    }

    push_message(L, &reply);
    return 1;
}

static int l_receive(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct message msg;
    uint64_t sender = 0;
    long status = kosmos_receive(cap, &msg, &sender);

    if (status != 0) {
        return fail(L, status);
    }

    push_message(L, &msg);

    /*
     * The sender comes back as a Lua integer here rather than as light
     * userdata, because that is what the kernel handed over: a raw pointer.
     * The kernel's own copy could hide it behind userdata; across the
     * boundary there is nothing to hide it in.
     *
     * That is the leak already recorded against sys_receive: a process
     * learns where a struct thread lives. It becomes a capability index at
     * M5, and then this is an index like any other.
     */
    lua_pushinteger(L, (lua_Integer)sender);
    return 2;
}

static int l_reply(lua_State *L)
{
    uint64_t sender = (uint64_t)luaL_checkinteger(L, 1);
    struct message msg;
    long status;

    take_message(L, 2, &msg);

    status = kosmos_reply(sender, &msg);
    if (status != 0) {
        return fail(L, status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg sys_functions[] = {
    { "write",    l_write },
    { "exit",     l_exit },
    { "yield",    l_yield },
    { "endpoint", l_endpoint },
    { "call",     l_call },
    { "receive",  l_receive },
    { "reply",    l_reply },
    { NULL, NULL }
};

int luaopen_sys(lua_State *L)
{
    luaL_newlib(L, sys_functions);
    return 1;
}
