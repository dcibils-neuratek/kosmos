/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to `posixtest.c` (`docs/w3.md`): a kit in no image but
 * `posixtest.elf`, reached as `use("posixtest.elf")`, as `cxxtest` is.
 * `run()` is the checks' own sentence.
 */

#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"

int kosmos_posixtest_run(char *out, unsigned long size);
int kosmos_storagetest_run(char *out, unsigned long size);
int kosmos_sockettest_run(char *out, unsigned long size, const char *host, int port,
                          int refused_port, int listen_port);

static void ends(unsigned long arg)
{
    kosmos_thread_exit((int)arg);
}

/* The index the kernel hands the next thread, collected at once: low
 * again once every detached thread has been collected. */
long kosmos_posixtest_fresh_index(void)
{
    long t = kosmos_thread_start(ends, 0);

    if (t >= 0) {
        (void)kosmos_thread_wait((unsigned long)t);
    }

    return t;
}

/* Whether the process table's entry for `id` is named `name` - so
 * `getpid` is held to what the table says, not to `SYS_SELF` again. */
int kosmos_posixtest_named(long id, const char *name)
{
    struct proc_info *all = calloc(512, sizeof(*all));
    long n, i;
    int found = 0;

    if (all == NULL) {
        return 0;
    }

    n = kosmos_proctable(all, 512);

    for (i = 0; i < n; i++) {
        if ((long)all[i].id == id && strncmp(all[i].name, name, sizeof(all[i].name)) == 0) {
            found = 1;
        }
    }

    free(all);
    return found;
}

static int l_run(lua_State *L)
{
    char said[1024];

    (void)kosmos_posixtest_run(said, sizeof(said));
    lua_pushstring(L, said);
    return 1;
}

static int l_storage(lua_State *L)
{
    char said[512];

    (void)kosmos_storagetest_run(said, sizeof(said));
    lua_pushstring(L, said);
    return 1;
}

/* `kit.sockets(host, port, refused_port, listen_port)`: the sockets'
 * checks against the harness's server (`sockettest.c`). */
static int l_sockets(lua_State *L)
{
    char said[512];

    (void)kosmos_sockettest_run(said, sizeof(said), luaL_checkstring(L, 1),
                                (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                                (int)luaL_checkinteger(L, 4));
    lua_pushstring(L, said);
    return 1;
}

void kosmos_posixtest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
    lua_pushcfunction(L, l_storage);
    lua_setfield(L, -2, "storage");
    lua_pushcfunction(L, l_sockets);
    lua_setfield(L, -2, "sockets");
}
