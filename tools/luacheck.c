/*
 * A syntax checker for the Lua in this repository, built for the host.
 *
 * Everything written in Lua here - init.lua, the programs in user/bin/, the
 * test and benchmark chunks - is loaded at run time inside the guest. A
 * mistake in any of them is therefore not a build failure: it is a process
 * that dies at boot, or a shell that vanishes the moment somebody types the
 * word that reaches the broken line. Those are expensive to diagnose,
 * because a dead process cannot say why it died - it prints by asking the
 * console server, and a dead process asks nothing.
 *
 * So the build compiles them here first, with the same Lua that will run
 * them, and refuses to go on if any of them will not parse. It catches
 * exactly what a compiler catches - syntax, not meaning - which is a small
 * class of mistake and one that has cost this project four separate
 * debugging sessions.
 *
 * Built from lua/upstream/ with the host compiler. It links no Kosmos code
 * and knows nothing about the system: it is a parser and nothing else.
 */

#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"

int main(int argc, char **argv)
{
    lua_State *L = luaL_newstate();
    int failures = 0;
    int i;

    if (L == NULL) {
        fprintf(stderr, "luacheck: no memory\n");
        return 2;
    }

    for (i = 1; i < argc; i++) {
        /* "t" and not "bt": this system refuses precompiled bytecode, and a
         * checker that accepted it would be checking something the guest
         * will not run. */
        if (luaL_loadfilex(L, argv[i], "t") != LUA_OK) {
            fprintf(stderr, "%s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
            failures++;
        } else {
            lua_pop(L, 1);
        }
    }

    lua_close(L);

    if (failures > 0) {
        fprintf(stderr, "luacheck: %d file(s) will not parse\n", failures);
        return 1;
    }

    return 0;
}
