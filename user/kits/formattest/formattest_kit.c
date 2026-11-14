/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to `formattest.cpp` (`docs/w5.md` W5f): a kit in no image but
 * `formattest.elf`, reached as `use("formattest.elf")`. `run(folder)`
 * returns the lines `formattest.cpp` says, a table of strings, for the
 * program to print: a kit has no `print` of its own, since a program's is
 * in its environment rather than the globals.
 *
 * The image names its own kit, as a project the C Kit built does
 * (`kosmos_project_name` and `kosmos_project_kit`, weak in
 * `user/init/sys_user.c`), so adding a kit like this one touches no file
 * every image is built from - and the gate, which runs everything when one
 * of those changes, need not (`docs/testing.md`).
 */

#include "lua.h"
#include "lauxlib.h"

#include "formattest.h"

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
    (void)formattest_run(folder, add_line, L);
    return 1;
}

const char kosmos_project_name[] = "formattest";

void kosmos_project_kit(lua_State *L);

void kosmos_project_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
}
