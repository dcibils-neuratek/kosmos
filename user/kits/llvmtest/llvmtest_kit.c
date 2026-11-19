/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to `llvmtest.cpp` (`docs/gpu3d.md` G1): a kit in no image but
 * `llvmtest.elf`, reached as `use("llvmtest.elf")`. `run(n)` is LLVM's
 * sentence - a function compiled at run time by the ORC JIT and called
 * with `n`, a second one in floating point, and the same module written
 * for the other processor.
 */

#include "lua.h"
#include "lauxlib.h"

int kosmos_llvmtest_run(long n, char *out, unsigned long size);

static int l_run(lua_State *L)
{
    static char said[1024];

    (void)kosmos_llvmtest_run((long)luaL_checkinteger(L, 1), said, sizeof(said));
    lua_pushstring(L, said);
    return 1;
}

void kosmos_llvmtest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
}
