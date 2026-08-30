/*
 * `sys`: the kernel, from the prompt.
 *
 * Everything M3 built - threads, the scheduler, capabilities, synchronous
 * IPC - reachable from Lua. It is the first taste of the property
 * `design.md` §9.1 is built around: the system is inspected and changed while
 * it is running, from the same language it is written in.
 *
 * It is deliberately *not* the interface Kosmos will have. At M4 Lua moves to
 * EL0 and these become real syscalls across a privilege boundary; at M5 the
 * inspection half of this disappears entirely into `/proc`, read through the
 * namespace protocol like any other resource, and `design.md` §9.5 is
 * emphatic that there should not be a second way to reach it. What is here is
 * a preview of the shape, not the shape itself.
 *
 * The one thing it does that is not a preview is share nothing. Every
 * spawned thread gets its own `lua_State`, because a `lua_State` is not
 * reentrant and two kernel threads inside one would corrupt it. That is not a
 * workaround: it is `design.md` §2's share-nothing userland arriving early
 * because the alternative does not work.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kosmos_lua.h"
#include "console.h"
#include "thread.h"
#include "sched.h"
#include "ipc.h"
#include "pmm.h"
#include "page.h"
#include "hal.h"
#include "serialize.h"

/* ------------------------------------------------------------------ */
/* Inspection                                                          */
/* ------------------------------------------------------------------ */

static const char *state_name(enum thread_state s)
{
    switch (s) {
    case THREAD_READY:   return "ready";
    case THREAD_RUNNING: return "running";
    case THREAD_BLOCKED: return "blocked";
    case THREAD_DEAD:    return "dead";
    default:             return "unused";
    }
}

static int sys_threads(lua_State *L)
{
    /*
     * What `/proc` will serve at M5, and what `design.md` §9.3's Monitor
     * reads. The shape is the same on purpose: an array of tables, each one
     * a record rather than a line of text to be parsed.
     */
    unsigned i;
    int n = 0;

    lua_newtable(L);

    for (i = 0; i < THREAD_MAX; i++) {
        const struct thread *t = thread_by_index(i);

        if (t == NULL) {
            continue;
        }

        lua_newtable(L);

        lua_pushinteger(L, (lua_Integer)t->id);
        lua_setfield(L, -2, "id");

        lua_pushstring(L, t->name);
        lua_setfield(L, -2, "name");

        lua_pushstring(L, state_name(t->state));
        lua_setfield(L, -2, "state");

        lua_pushinteger(L, (lua_Integer)t->switches);
        lua_setfield(L, -2, "switches");

        lua_pushboolean(L, t == thread_current());
        lua_setfield(L, -2, "current");

        lua_rawseti(L, -2, ++n);
    }

    return 1;
}

static int sys_memory(lua_State *L)
{
    lua_newtable(L);

    lua_pushinteger(L, (lua_Integer)pmm_total_pages());
    lua_setfield(L, -2, "pages_total");

    lua_pushinteger(L, (lua_Integer)pmm_free_pages());
    lua_setfield(L, -2, "pages_free");

    lua_pushinteger(L, (lua_Integer)(PAGE_SIZE));
    lua_setfield(L, -2, "page_size");

    lua_pushinteger(L, (lua_Integer)heap_size());
    lua_setfield(L, -2, "heap_size");

    lua_pushinteger(L, (lua_Integer)heap_used());
    lua_setfield(L, -2, "heap_used");

    return 1;
}

static int sys_ticks(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)hal_ticks());
    return 1;
}

static int sys_scheduler(lua_State *L)
{
    lua_pushstring(L, sched_current()->name);
    return 1;
}

static int sys_yield(lua_State *L)
{
    (void)L;
    thread_yield();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Spawning                                                            */
/* ------------------------------------------------------------------ */

/*
 * Handed to a new thread and owned by it. The source has to be copied out of
 * the parent's heap: it is a Lua string over there, and the parent's
 * collector is free to reclaim it the moment the call returns.
 */
/*
 * What a spawned thread is handed: its source, and the capabilities it was
 * given, in the order they were granted.
 *
 * Several rather than one, because a process needs several. A server needs
 * the endpoint it listens on; a client needs the one it talks to and the
 * console it prints to. Granting one and making the rest arrive later means
 * a window where the thread is running and cannot do its job.
 */
#define SPAWN_CAPS_MAX  8

struct spawn_args {
    char   *source;
    cap_t   caps[SPAWN_CAPS_MAX];   /* the child's own indices */
    unsigned ncaps;
    char    name[THREAD_NAME_MAX];
};

static void lua_thread_main(void *arg)
{
    struct spawn_args *a = arg;
    lua_State *L = kosmos_lua_open();

    if (L == NULL) {
        kputs("spawn: no memory for a lua_State\n");
    } else {
        int status = luaL_loadbufferx(L, a->source, strlen(a->source),
                                      a->name, "t");

        if (status == LUA_OK) {
            /* The chunk's `...` is its capabilities, in the order they were
             * granted, and they are the only things it was handed and
             * therefore the only things it can reach. That is the capability
             * model, expressed as arguments. */
            unsigned i;

            for (i = 0; i < a->ncaps; i++) {
                lua_pushinteger(L, (lua_Integer)a->caps[i]);
            }

            status = lua_pcall(L, (int)a->ncaps, 0, 0);
        }

        if (status != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            kputs("\n");
            kputs(a->name);
            kputs(": ");
            kputs((msg != NULL) ? msg : "(no message)");
            kputs("\n");
        }

        lua_close(L);
    }

    free(a->source);
    free(a);
}

static int sys_spawn(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    size_t len;
    const char *source = luaL_checklstring(L, 2, &len);
    int nshare = lua_gettop(L) - 2;
    struct spawn_args *a;
    struct thread *t;
    int i;

    if (nshare < 0) {
        nshare = 0;
    }

    if (nshare > SPAWN_CAPS_MAX) {
        lua_pushnil(L);
        lua_pushstring(L, "too many capabilities");
        return 2;
    }

    a = malloc(sizeof(*a));
    if (a == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }

    a->source = malloc(len + 1);
    if (a->source == NULL) {
        free(a);
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }

    memcpy(a->source, source, len);
    a->source[len] = '\0';
    a->ncaps = 0;

    /* Truncated rather than rejected: a name is for reading in `sys.threads`
     * and nothing depends on it being complete. */
    {
        size_t i;
        for (i = 0; i + 1 < THREAD_NAME_MAX && name[i] != '\0'; i++) {
            a->name[i] = name[i];
        }
        a->name[i] = '\0';
    }

    t = thread_create(a->name, lua_thread_main, a);

    if (t == NULL) {
        free(a->source);
        free(a);
        lua_pushnil(L);
        lua_pushstring(L, "no free thread slots");
        return 2;
    }

    /*
     * Granted after the thread exists, because a child's index is only
     * meaningful once there is a table to put it in, and before it runs,
     * because a thread that runs first finds an empty table. Nothing has to
     * be re-plumbed: the thread was handed the pointer, not a copy.
     *
     * Each index is the child's own and unrelated to the parent's. An index
     * means something only inside the table it came from, which is what
     * stops one being guessed or forged somewhere else.
     */
    for (i = 0; i < nshare; i++) {
        cap_t granted = ipc_cap_grant(t, (cap_t)luaL_checkinteger(L, 3 + i));

        if (granted < 0) {
            /* The thread exists and would run without something it was meant
             * to have, which is a well-defined and useless state. */
            lua_pushnil(L);
            lua_pushstring(L, "could not grant a capability");
            return 2;
        }

        a->caps[a->ncaps++] = granted;
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* ------------------------------------------------------------------ */
/* IPC                                                                 */
/* ------------------------------------------------------------------ */

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
 * A capability travels alongside the value, never inside it.
 *
 * An index means something only in the table it came from, so a capability
 * serialised as data would arrive as a number naming something else. It is
 * passed as its own argument and comes back as its own return value, which
 * also makes it visible at every call site that one is being handed over.
 */
static void take_message(lua_State *L, int index, struct message *m,
                         int cap_arg)
{
    int rc;

    m->tag = 0;
    m->length = 0;
    message_set_cap(m, (cap_t)luaL_optinteger(L, cap_arg, -1));

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

static const char *ipc_error(int status)
{
    switch (status) {
    case IPC_ERR_BAD_CAP:  return "no such capability";
    case IPC_ERR_GONE:     return "the endpoint was destroyed";
    case IPC_ERR_NO_PEER:  return "that thread is not waiting for a reply";
    case IPC_ERR_NO_SPACE: return "out of endpoints or capability slots";
    default:               return "unknown IPC error";
    }
}

static int fail(lua_State *L, int status)
{
    lua_pushnil(L);
    lua_pushstring(L, ipc_error(status));
    return 2;
}

static int sys_endpoint(lua_State *L)
{
    cap_t cap = ipc_endpoint_create();

    if (cap < 0) {
        return fail(L, cap);
    }

    lua_pushinteger(L, (lua_Integer)cap);
    return 1;
}

static int sys_destroy(lua_State *L)
{
    cap_t cap = (cap_t)luaL_checkinteger(L, 1);
    int status = ipc_endpoint_destroy(cap);

    if (status != IPC_OK) {
        return fail(L, status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int sys_call(lua_State *L)
{
    cap_t cap = (cap_t)luaL_checkinteger(L, 1);
    struct message msg;
    struct message reply;
    int status;

    take_message(L, 2, &msg, 3);

    /* Blocks this kernel thread until the far side answers, which is the
     * point: a client that has to poll is not synchronous IPC. */
    status = ipc_call(cap, &msg, &reply);

    if (status != IPC_OK) {
        return fail(L, status);
    }

    push_message(L, &reply);
    lua_pushinteger(L, (lua_Integer)message_get_cap(&reply));
    return 2;
}

static int sys_receive(lua_State *L)
{
    cap_t cap = (cap_t)luaL_checkinteger(L, 1);
    struct message msg;
    struct thread *sender = NULL;
    int status = ipc_receive(cap, &msg, &sender);

    if (status != IPC_OK) {
        return fail(L, status);
    }

    push_message(L, &msg);

    /*
     * The sender comes back as light userdata: a token to hand to
     * `sys.reply` and nothing else. It is deliberately not a capability and
     * deliberately not a number, so it cannot be stored, forged, or arrived
     * at by arithmetic.
     */
    lua_pushlightuserdata(L, sender);
    lua_pushinteger(L, (lua_Integer)message_get_cap(&msg));
    return 3;
}

static int sys_reply(lua_State *L)
{
    struct thread *sender;
    struct message msg;
    int status;

    luaL_checktype(L, 1, LUA_TLIGHTUSERDATA);
    sender = lua_touserdata(L, 1);

    take_message(L, 2, &msg, 3);

    status = ipc_reply(sender, &msg);

    if (status != IPC_OK) {
        return fail(L, status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* ------------------------------------------------------------------ */

static const luaL_Reg sys_functions[] = {
    { "threads",   sys_threads },
    { "memory",    sys_memory },
    { "ticks",     sys_ticks },
    { "scheduler", sys_scheduler },
    { "yield",     sys_yield },
    { "spawn",     sys_spawn },
    { "endpoint",  sys_endpoint },
    { "destroy",   sys_destroy },
    { "call",      sys_call },
    { "receive",   sys_receive },
    { "reply",     sys_reply },
    { NULL, NULL }
};

int luaopen_sys(lua_State *L)
{
    luaL_newlib(L, sys_functions);
    return 1;
}
