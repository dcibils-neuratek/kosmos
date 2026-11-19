/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to Khronos's Vulkan conformance suite (`docs/gpu3d.md` G2): a
 * kit in no image but `vkcts.elf`, reached as `use("vkcts.elf")`, with the
 * CTS - dEQP-VK's `info` and `api` groups, `runtime/patches/cts/0003` - and
 * lavapipe linked in.
 *
 *     kit.run({"--deqp-case=dEQP-VK.api.smoke.*", ...})
 *
 * takes the suite's own options, as its command line would, and answers
 * with a table of what the run counted - executed, passed, failed,
 * not_supported, warnings, waived, device_lost, and complete - or with nil
 * and the framework's sentence when it stopped before the end. Every case's
 * result is in the log the options name.
 */

#include "lua.h"
#include "lauxlib.h"

enum { COUNTS = 8, MOST = 32 };

int kosmos_vkcts_run(int argc, char **argv, int counts[COUNTS], char *why, unsigned long why_size);

static const char *const names[COUNTS] = {
    "executed", "passed", "failed", "not_supported", "warnings", "waived", "device_lost", "complete",
};

static int l_run(lua_State *L)
{
    char *argv[MOST + 2];
    int counts[COUNTS];
    char why[256];
    int argc = 0;

    luaL_checktype(L, 1, LUA_TTABLE);
    argv[argc++] = (char *)"deqp-vk";
    for (lua_Integer i = 1; ; i++) {
        if (lua_rawgeti(L, 1, i) == LUA_TNIL) {
            lua_pop(L, 1);
            break;
        }
        if (argc > MOST)
            return luaL_error(L, "vkcts: at most %d options", MOST);
        /* The strings stay on the stack, and alive, until the run is over. */
        argv[argc++] = (char *)luaL_checkstring(L, -1);
    }
    argv[argc] = NULL;

    if (kosmos_vkcts_run(argc, argv, counts, why, sizeof(why)) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }
    lua_createtable(L, 0, COUNTS);
    for (int i = 0; i < COUNTS - 1; i++) {
        lua_pushinteger(L, counts[i]);
        lua_setfield(L, -2, names[i]);
    }
    lua_pushboolean(L, counts[COUNTS - 1]);
    lua_setfield(L, -2, names[COUNTS - 1]);
    return 1;
}

void kosmos_vkcts_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
}
