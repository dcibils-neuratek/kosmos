/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A kit's way to the program that called it (`kitenv.h`).
 */

#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kits/kitenv.h"

int kit_program_has(lua_State *L, const char *want)
{
    lua_Debug ar;

    for (int level = 0; lua_getstack(L, level, &ar); level++) {
        lua_getinfo(L, "f", &ar);

        for (int n = 1;; n++) {
            const char *name = lua_getupvalue(L, -1, n);

            if (name == NULL) {
                break;
            }

            if (strcmp(name, "_ENV") == 0 && lua_istable(L, -1)) {
                lua_getfield(L, -1, want);

                if (!lua_isnil(L, -1)) {
                    lua_replace(L, -3);         /* over the function */
                    lua_pop(L, 1);              /* the environment */
                    return 1;
                }

                lua_pop(L, 1);
            }

            lua_pop(L, 1);
        }

        lua_pop(L, 1);
    }

    return 0;
}

int kit_library(lua_State *L, int *ref, const char *path, char *why, size_t why_bytes)
{
    if (*ref == LUA_NOREF) {
        if (!kit_program_has(L, "use")) {
            strncpy(why, "no program's environment above this C to use a library from", why_bytes - 1);
            why[why_bytes - 1] = '\0';
            return 0;
        }

        lua_pushstring(L, path);

        if (lua_pcall(L, 1, 1, 0) != LUA_OK || !lua_istable(L, -1)) {
            strncpy(why, lua_isstring(L, -1) ? lua_tostring(L, -1) : "a library would not load",
                    why_bytes - 1);
            why[why_bytes - 1] = '\0';
            lua_pop(L, 1);
            return 0;
        }

        *ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
    return 1;
}

long kit_capability(lua_State *L, const char *path)
{
    int top = lua_gettop(L);
    long cap = -1;

    if (kit_program_has(L, "fs")) {
        lua_getfield(L, -1, "capability");
        lua_pushstring(L, path);

        if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isinteger(L, -1)) {
            cap = (long)lua_tointeger(L, -1);
        }
    }

    lua_settop(L, top);
    return cap;
}
