/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to `icutest.cpp` (`docs/w4.md` W4b): a kit in no image but
 * `icutest.elf`, reached as `use("icutest.elf")`. `run()` is ICU's
 * sentence - what it normalised, collated, broke and formatted.
 */

#include "lua.h"
#include "lauxlib.h"

int kosmos_icutest_run(char *out, unsigned long size);

static int l_run(lua_State *L)
{
    static char said[2048];

    (void)kosmos_icutest_run(said, sizeof(said));
    lua_pushstring(L, said);
    return 1;
}

void kosmos_icutest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
}
