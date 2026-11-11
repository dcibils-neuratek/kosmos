/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to `posixtest.c` (`docs/w3.md`): a kit in no image but
 * `posixtest.elf`, reached as `use("posixtest.elf")`, as `cxxtest` is.
 * `run()` is the checks' own sentence.
 */

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"

int kosmos_posixtest_run(char *out, unsigned long size);

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

static int l_run(lua_State *L)
{
    char said[256];

    (void)kosmos_posixtest_run(said, sizeof(said));
    lua_pushstring(L, said);
    return 1;
}

void kosmos_posixtest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
}
