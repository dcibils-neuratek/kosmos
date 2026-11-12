/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to `wtftest.cpp` (`docs/w4.md` W4c): a kit in no image but
 * `wtftest.elf`, reached as `use("wtftest.elf")`. `run()` is WTF's
 * sentence - its text, a hash map, threads, a thread stopped, the run
 * loop, rounding and files.
 */

#include "lua.h"
#include "lauxlib.h"

int kosmos_wtftest_run(char *out, unsigned long size);

static int l_run(lua_State *L)
{
    static char said[2048];

    (void)kosmos_wtftest_run(said, sizeof(said));
    lua_pushstring(L, said);
    return 1;
}

void kosmos_wtftest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
}
