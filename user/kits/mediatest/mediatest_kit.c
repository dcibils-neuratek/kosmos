/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to `mediatest.c` (`docs/media.md` M1): a kit in no image but
 * `mediatest.elf`, reached as `use("mediatest.elf")`. `run(folder)`
 * returns the lines `mediatest.c` says, a table of strings, for the
 * program to print. The image names its own kit (`kosmos_project_name`,
 * weak in `user/init/sys_user.c`), as `formattest.elf` does.
 */

#include "lua.h"
#include "lauxlib.h"

#include "mediatest.h"

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
    (void)mediatest_run(folder, add_line, L);
    return 1;
}

const char kosmos_project_name[] = "mediatest";

void kosmos_project_kit(lua_State *L);

void kosmos_project_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
}
