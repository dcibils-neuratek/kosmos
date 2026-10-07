/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* sum.c - the loop sum.lua also runs, in C. */

#include "kosmos_kit.h"

/* sum.sum(n): the sum of i % 7 for i = 1 .. n */
static int l_sum(lua_State *L)
{
    lua_Integer n = luaL_checkinteger(L, 1), total = 0;

    for (lua_Integer i = 1; i <= n; i++) {
        total += i % 7;
    }

    lua_pushinteger(L, total);
    return 1;
}

KOSMOS_KIT(sum)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_sum);
    lua_setfield(L, -2, "sum");
}
