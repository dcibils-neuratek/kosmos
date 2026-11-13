/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to `webcoretest.cpp` (`docs/w6.md` W6b): a kit in no image but
 * `webcoretest.elf`, reached as `use("webcoretest.elf")`.
 * `run(folder, {page, ...})` returns the lines `webcoretest.cpp` says, a
 * table of strings, for the program to print.
 */

#include "lua.h"
#include "lauxlib.h"

#include "webcoretest.h"

/* Enough for the suite's pages; more is refused, not truncated. */
#define MOST_PAGES 64

static void add_line(const char *line, void *ctx)
{
    lua_State *L = ctx;

    lua_pushstring(L, line);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
}

static int l_run(lua_State *L)
{
    const char *folder = luaL_checkstring(L, 1);
    const char *pages[MOST_PAGES];
    lua_Integer count, i;

    luaL_checktype(L, 2, LUA_TTABLE);
    count = (lua_Integer)lua_rawlen(L, 2);
    luaL_argcheck(L, count <= MOST_PAGES, 2, "too many pages");
    for (i = 1; i <= count; i++) {
        lua_rawgeti(L, 2, i);
        pages[i - 1] = luaL_checkstring(L, -1);
        /* The string stays alive in the argument table. */
        lua_pop(L, 1);
    }

    lua_newtable(L);
    (void)webcoretest_run(folder, pages, (int)count, add_line, L);
    return 1;
}

void kosmos_webcoretest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
}
