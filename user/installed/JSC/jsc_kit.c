/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to JavaScriptCore (`docs/w4.md` W4d), in no image but
 * `jsc.elf`, reached as `use("jsc.elf")`:
 *
 *   run(path)    the script in that file
 *   eval(text)   the script given
 *   t262(bundle) test262's cases, read; how many (`docs/w4.md` W4e)
 *   case(n)      the nth of them run: true, or false and why
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

long kosmos_jsc_t262_open(const char *path);
int kosmos_jsc_t262_case(long index, char **out, unsigned long *length);

static int l_t262(lua_State *L)
{
    long count = kosmos_jsc_t262_open(luaL_checkstring(L, 1));

    if (count < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "not a test262 bundle");
        return 2;
    }
    lua_pushinteger(L, count);
    return 1;
}

static int l_case(lua_State *L)
{
    char *out;
    unsigned long length;
    int why = kosmos_jsc_t262_case((long)luaL_checkinteger(L, 1), &out, &length);

    lua_pushboolean(L, why == 0);
    lua_pushlstring(L, out, length);
    return 2;
}

long kosmos_jsc_jit_pages(void);

unsigned long kosmos_jsc_jit_bytes(void);

/* The JIT's pool (`docs/jit.md`): its pages written, as Nebula counts them,
 * and the bytes the JIT wrote into it. */
static int l_jit_pages(lua_State *L)
{
    lua_pushinteger(L, kosmos_jsc_jit_pages());
    lua_pushinteger(L, (lua_Integer)kosmos_jsc_jit_bytes());
    return 2;
}

void kosmos_jsc_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
    lua_pushcfunction(L, l_eval);
    lua_setfield(L, -2, "eval");
    lua_pushcfunction(L, l_t262);
    lua_setfield(L, -2, "t262");
    lua_pushcfunction(L, l_case);
    lua_setfield(L, -2, "case");
    lua_pushcfunction(L, l_jit_pages);
    lua_setfield(L, -2, "jit_pages");
}
