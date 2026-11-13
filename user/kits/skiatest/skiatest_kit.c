/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to `skiatest.cpp` (`docs/w5.md` W5e): a kit in no image but
 * `skiatest.elf`, reached as `use("skiatest.elf")`. `run(folder)`
 * returns the lines `skiatest.cpp` says, a table of strings, for the
 * program to print.
 */

#include "lua.h"
#include "lauxlib.h"

#include "skiatest.h"

static void add_line(const char *line, void *ctx)
{
    lua_State *L = ctx;

    lua_pushstring(L, line);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
}

static int l_run(lua_State *L)
{
    const char *folder = luaL_checkstring(L, 1);

    lua_newtable(L);
    (void)skiatest_run(folder, add_line, L);
    return 1;
}

void kosmos_skiatest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
}
