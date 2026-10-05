/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A kit that is in no image but its own - the loader's test (`docs/elf.md`,
 * steps 3 and 4).
 *
 * The system's image does not link it, and it is an application's own kit
 * (`own` in `sys_user.c`'s list) rather than one Kosmos ships, so it is
 * never `/Kosmos/Kits/apptest`. `build/.../apps/apptest.elf` is the
 * system's objects and this one, and a program run in that image reaches
 * it as `use("apptest.elf")`. An answer of 42 is therefore a program that
 * ran in an image loaded from a file, and nothing else could have said it.
 */

#include "lua.h"
#include "lauxlib.h"

static int l_answer(lua_State *L)
{
    lua_pushinteger(L, 42);
    return 1;
}

void kosmos_apptest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_answer);
    lua_setfield(L, -2, "answer");
}
