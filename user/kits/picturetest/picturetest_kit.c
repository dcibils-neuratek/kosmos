/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to `picturetest.c` (`docs/w5.md` W5a): a kit in no image but
 * `picturetest.elf`, reached as `use("picturetest.elf")`. `run(folder)`
 * returns the lines `picturetest.c` says, a table of strings, for the
 * program to print: a kit has no `print` of its own, since a program's is
 * in its environment rather than the globals.
 */

#include "lua.h"
#include "lauxlib.h"

#include "picturetest.h"

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
    (void)picturetest_run(folder, add_line, L);
    return 1;
}

void kosmos_picturetest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
}
