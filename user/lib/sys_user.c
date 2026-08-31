/*
 * `sys`, from inside a process.
 *
 * The only one there is. The kernel had a copy of this table once, over
 * direct calls rather than syscalls, and servers were kernel threads; both
 * are gone, and the names that survived the move are the ones that could be
 * expressed as a syscall. That the two were interchangeable for a milestone
 * is what let the servers walk out one at a time instead of all at once.
 *
 * The inspection half did not survive, and deliberately. `sys.threads` and
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

/*
 * A capability travels alongside the value, never inside it. See the kernel's
 * copy: an index means something only in the table it came from.
 */
static void take_message(lua_State *L, int index, struct message *m,
                         int cap_arg)
{
    int rc;
    long c = (long)luaL_optinteger(L, cap_arg, -1);

    m->tag = 0;
    m->length = 0;
    m->cap_plus_one = (c >= 0) ? (uint32_t)(c + 1) : 0u;

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

static int l_getchar(lua_State *L)
{
    long c = kosmos_getchar();

    if (c < 0) {
        lua_pushnil(L);         /* nothing waiting, which is not an error */
        return 1;
    }

    lua_pushinteger(L, (lua_Integer)c);
    return 1;
}

static int l_spawn(lua_State *L)
{
    unsigned long arg = (unsigned long)luaL_checkinteger(L, 1);
    unsigned long flags = (unsigned long)luaL_optinteger(L, 3, 0);
    int caps[16];
    unsigned long n = 0;
    long id;

    /* Capabilities as an array, in the order the child will see them. */
    if (!lua_isnoneornil(L, 2)) {
        lua_Integer count;

        luaL_checktype(L, 2, LUA_TTABLE);
        count = (lua_Integer)lua_rawlen(L, 2);

        if (count > 16) {
            lua_pushnil(L);
            lua_pushstring(L, "too many capabilities");
            return 2;
        }

        for (n = 0; n < (unsigned long)count; n++) {
            lua_rawgeti(L, 2, (lua_Integer)(n + 1));
            caps[n] = (int)luaL_checkinteger(L, -1);
            lua_pop(L, 1);
        }
    }

    id = kosmos_spawn(arg, caps, n, flags);

    if (id < 0) {
        lua_pushnil(L);
        lua_pushstring(L, (id == -102) ? "this process does not hold that device"
                                       : "could not spawn");
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

static int l_wait(lua_State *L)
{
    uint64_t id = 0;
    int nonblocking = lua_toboolean(L, 1);
    long code = kosmos_wait(&id, nonblocking);

    if (code == -106) {
        lua_pushnil(L);
        lua_pushstring(L, "no child ready");
        return 2;
    }

    if (code < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no children");
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)id);
    lua_pushinteger(L, (lua_Integer)code);
    return 2;
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

/*
 * How long something took, in counter ticks.
 *
 * A Lua integer is 64 bits, so the counter fits without losing a bit, and
 * the difference between two of these is the only thing anybody should use
 * it for. It is not a date: `design.md` §4.4 makes that a capability.
 */
static int l_ticks(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)kosmos_ticks());
    return 1;
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

static int l_destroy(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    long status = kosmos_endpoint_destroy(cap);

    if (status != 0) {
        return fail(L, status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_call(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct message msg;
    struct message reply;
    long status;

    take_message(L, 2, &msg, 3);

    status = kosmos_call(cap, &msg, &reply);
    if (status != 0) {
        return fail(L, status);
    }

    push_message(L, &reply);
    lua_pushinteger(L, (lua_Integer)((reply.cap_plus_one == 0)
                                     ? -1 : (long)reply.cap_plus_one - 1));
    return 2;
}

static int l_receive(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct message msg;
    uint64_t sender = 0;
    int nonblocking = lua_toboolean(L, 2);
    long status = kosmos_receive(cap, &msg, &sender, nonblocking);

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
    lua_pushinteger(L, (lua_Integer)((msg.cap_plus_one == 0)
                                     ? -1 : (long)msg.cap_plus_one - 1));
    return 3;
}

static int l_reply(lua_State *L)
{
    uint64_t sender = (uint64_t)luaL_checkinteger(L, 1);
    struct message msg;
    long status;

    take_message(L, 2, &msg, 3);

    status = kosmos_reply(sender, &msg);
    if (status != 0) {
        return fail(L, status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/*
 * A value, as bytes, without sending it anywhere.
 *
 * The same serialiser a message uses, on its own. Two reasons it is exposed
 * rather than staying an internal detail of `sys.call`:
 *
 *   - a value has to be storable and not only sendable. At M8 `fs.write`
 *     with a table is this operation with a different destination, and it
 *     should not have to invent a second encoding to get there.
 *   - it is what a benchmark can hold still. The cost of the serialiser is
 *     `design.md` §1's thesis priced in ticks, and pricing it through a
 *     round trip would be measuring the IPC path instead.
 *
 * Bounded by a message, because a message is the buffer it writes into. A
 * value larger than one is refused rather than truncated.
 */
static int l_pack(lua_State *L)
{
    struct message m;
    int rc;

    luaL_checkany(L, 1);

    m.tag = 0;
    m.cap_plus_one = 0;
    m.length = 0;

    rc = serialize_pack(L, 1, &m);

    if (rc != SERIALIZE_OK) {
        lua_pushnil(L);
        lua_pushstring(L, serialize_error(rc));
        return 2;
    }

    lua_pushlstring(L, (const char *)m.data, m.length);
    return 1;
}

static int l_unpack(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    struct message m;
    int rc;

    if (len > MSG_BYTES) {
        lua_pushnil(L);
        lua_pushstring(L, "longer than a message");
        return 2;
    }

    /*
     * Into a message rather than read in place, because serialize_unpack
     * takes one. The copy is what the caller would pay anyway if this had
     * come off a wire, and it keeps one definition of the reader.
     */
    m.tag = 0;
    m.cap_plus_one = 0;
    m.length = (uint32_t)len;
    memcpy(m.data, s, len);

    rc = serialize_unpack(L, &m);

    if (rc != SERIALIZE_OK) {
        lua_pushnil(L);
        lua_pushstring(L, serialize_error(rc));
        return 2;
    }

    return 1;
}

/*
 * The machine, as a table.
 *
 * Straight out of the syscall with no interpretation: raw ID registers, pool
 * counts, device geometry. What any of it *means* is decided in Lua, in
 * `init.lua`'s device server, because decoding a MIDR is a table lookup and
 * tables belong up here - a processor this kernel has never heard of gets
 * described properly without the kernel changing.
 */
static int l_info(lua_State *L)
{
    struct sysinfo info;

    if (kosmos_sysinfo(&info) < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "the kernel refused to describe itself");
        return 2;
    }

    lua_createtable(L, 0, 24);

#define SET(name, value) \
    do { lua_pushinteger(L, (lua_Integer)(value)); \
         lua_setfield(L, -2, name); } while (0)

    SET("midr",             info.midr);
    SET("mpidr",            info.mpidr);
    SET("ctr",              info.ctr);
    SET("pfr0",             info.pfr0);
    SET("isar0",            info.isar0);
    SET("mmfr0",            info.mmfr0);
    SET("counter_hz",       info.counter_hz);

    SET("ram_base",         info.ram_base);
    SET("ram_size",         info.ram_size);
    SET("pages_total",      info.pages_total);
    SET("pages_free",       info.pages_free);
    SET("page_size",        info.page_size);

    SET("threads_used",     info.threads_used);
    SET("threads_total",    info.threads_total);
    SET("processes_used",   info.processes_used);
    SET("processes_held",   info.processes_held);
    SET("processes_total",  info.processes_total);
    SET("endpoints_used",   info.endpoints_used);
    SET("endpoints_total",  info.endpoints_total);
    SET("spaces_used",      info.spaces_used);
    SET("spaces_total",     info.spaces_total);

    SET("screen_width",     info.screen_width);
    SET("screen_height",    info.screen_height);
    SET("screen_pitch",     info.screen_pitch);
    SET("has_keyboard",     info.has_keyboard);

    SET("idle_ticks",       info.idle_ticks);
    SET("busy_ticks",       info.busy_ticks);
    SET("cpus",             info.cpus);
    SET("tick_hz",          info.tick_hz);
    SET("current_el",       info.current_el);

#undef SET

    return 1;
}

static int l_setname(lua_State *L)
{
    size_t len;
    const char *name = luaL_checklstring(L, 1, &len);

    lua_pushboolean(L, kosmos_setname(name, len) == 0);
    return 1;
}

/*
 * Every process, as a list of tables.
 *
 * Raw again: an id, a name the process chose, a state number, ticks that
 * only rise. What a name means and which layer it belongs to is decided by
 * whoever asked - the kernel has no opinion about whether something is a
 * server or an app, and should not acquire one.
 */
static int l_processes(lua_State *L)
{
    struct proc_info table[32];
    long n = kosmos_proctable(table, 32);
    long i;

    if (n < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "the kernel would not say");
        return 2;
    }

    lua_createtable(L, (int)n, 0);

    for (i = 0; i < n; i++) {
        lua_createtable(L, 0, 9);

#define SETI(k, v) do { lua_pushinteger(L, (lua_Integer)(v)); \
                        lua_setfield(L, -2, k); } while (0)
        SETI("id",        table[i].id);
        SETI("state",     table[i].state);
        SETI("exit_code", table[i].exit_code);
        SETI("ticks",     table[i].ticks);
        SETI("pages",     table[i].pages);
        SETI("caps",      table[i].caps);
        SETI("owns",      table[i].owns);
#undef SETI

        lua_pushboolean(L, table[i].exited != 0);
        lua_setfield(L, -2, "exited");

        lua_pushstring(L, table[i].name);
        lua_setfield(L, -2, "name");

        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }

    return 1;
}

/*
 * The programs carried in this image, as Lua source for a chunk that
 * returns a table of name to source.
 *
 * **Not a syscall.** It is data in the image, reached through this table
 * because that is where a process looks for things it did not bring with
 * it. Only the /bin server calls it, which is deliberate: the string is a
 * few kilobytes and every state that asked for it would keep a copy.
 *
 * There is no disk. Until M8 this is where a program lives.
 */
extern const char programs_lua[];
extern const char libraries_lua[];

static int l_pointer(lua_State *L)
{
    struct pointer_info info;
    long status = kosmos_pointer(&info);

    if (status != 0) {
        return fail(L, status);
    }

#define PUT(name, value) do {                       \
        lua_pushinteger(L, (lua_Integer)(value));   \
        lua_setfield(L, -2, (name));                \
    } while (0)

    lua_createtable(L, 0, 8);
    PUT("x", info.x);
    PUT("y", info.y);
    PUT("min_x", info.min_x);
    PUT("max_x", info.max_x);
    PUT("min_y", info.min_y);
    PUT("max_y", info.max_y);
    PUT("buttons", info.buttons);

#undef PUT

    lua_pushboolean(L, info.moved != 0);
    lua_setfield(L, -2, "moved");

    return 1;
}

static int l_programs(lua_State *L)
{
    lua_pushstring(L, programs_lua);
    return 1;
}

static int l_libraries(lua_State *L)
{
    lua_pushstring(L, libraries_lua);
    return 1;
}

static const luaL_Reg sys_functions[] = {
    { "write",    l_write },
    { "getchar",  l_getchar },
    { "spawn",    l_spawn },
    { "wait",     l_wait },
    { "exit",     l_exit },
    { "yield",    l_yield },
    { "ticks",    l_ticks },
    { "info",     l_info },
    { "name",     l_setname },
    { "processes", l_processes },
    { "pointer",  l_pointer },
    { "programs", l_programs },
    { "libraries", l_libraries },
    { "endpoint", l_endpoint },
    { "destroy",  l_destroy },
    { "call",     l_call },
    { "receive",  l_receive },
    { "reply",    l_reply },
    { "pack",     l_pack },
    { "unpack",   l_unpack },
    { NULL, NULL }
};

int luaopen_sys(lua_State *L)
{
    luaL_newlib(L, sys_functions);
    return 1;
}
