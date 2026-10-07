/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * primes.c - a C app: counts the primes below a limit with a sieve, and
 * says how many. Run it with a limit - `./primes.lua
 * 1000000` at the prompt, or F5 in the IDE for ten million.
 */

#include <stdlib.h>

#include "kosmos_kit.h"

/* main(args): what the program was started with, as one string. */
static int l_main(lua_State *L)
{
    const char *args = luaL_optstring(L, 1, "");
    long limit = strtol(args, NULL, 10);
    char *composite;
    long count = 0;

    if (limit < 2) {
        limit = 10000000;
    }

    composite = calloc((size_t)limit, 1);
    if (composite == NULL) {
        return luaL_error(L, "primes: no memory for %ld numbers", limit);
    }

    for (long i = 2; i < limit; i++) {
        if (!composite[i]) {
            count++;

            for (long j = i * i; j < limit; j += i) {
                composite[j] = 1;
            }
        }
    }

    free(composite);

    /* What it says, handed back: the program's Lua line prints it, where
     * its console is. */
    lua_pushfstring(L, "%d primes below %d", (int)count, (int)limit);
    return 1;
}

KOSMOS_KIT(primes)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_main);
    lua_setfield(L, -2, "main");
}
