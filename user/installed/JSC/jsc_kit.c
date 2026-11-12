/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to JavaScriptCore (`docs/w4.md` W4d), in no image but
 * `jsc.elf`, reached as `use("jsc.elf")`:
 *
 *   run(path)    the script in that file
 *   eval(text)   the script given
 *
 * Each returns what the script printed and whether it finished without
 * throwing. A file's bytes go from the disk to the engine without passing
 * through Lua.
 */

#include "lua.h"
#include "lauxlib.h"

int kosmos_jsc_run(const char *path, char **out, unsigned long *length);
int kosmos_jsc_eval(const char *source, unsigned long size, char **out, unsigned long *length);

static int l_run(lua_State *L)
{
    char *out;
    unsigned long length;
    int why = kosmos_jsc_run(luaL_checkstring(L, 1), &out, &length);

    lua_pushlstring(L, out, length);
    lua_pushboolean(L, why == 0);
    return 2;
}

static int l_eval(lua_State *L)
{
    size_t size;
    const char *source = luaL_checklstring(L, 1, &size);
    char *out;
    unsigned long length;
    int why = kosmos_jsc_eval(source, size, &out, &length);

    lua_pushlstring(L, out, length);
    lua_pushboolean(L, why == 0);
    return 2;
}

void kosmos_jsc_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
    lua_pushcfunction(L, l_eval);
    lua_setfield(L, -2, "eval");
}
