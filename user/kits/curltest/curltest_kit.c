/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to W5d's curl test (`docs/w5.md`): a kit in no image but
 * `curltest.elf`, reached as `use("curltest.elf")`.
 *
 *     code = kit.run(args, log)
 *
 * runs curl's own program - `src/` of the release, unmodified, built by
 * curl's CMake with the objects of its `curl` program and its `main`
 * renamed (`tools/w5_build.py`) - with that argument list, what it prints
 * going to the file `log`, and gives back what its `main` returned: 0, or
 * one of curl's own codes (60, a certificate it was right to refuse).
 *
 * **One run a process**, as `ssltest`'s: a script of its own from the
 * prompt for each command (`tools/run_curl.py`). curl's program keeps its
 * options in globals it sets up and tears down once.
 */

#include <errno.h>
#include <posix_program.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

/* curl's `main`, renamed as its objects are archived. */
int curltest_curl_main(int argc, char **argv);

/* curl's program is a desktop's: the stack it was written for. */
#define PROGRAM_STACK_PAGES 512

static int l_run(lua_State *L)
{
    const char *log = luaL_checkstring(L, 2);
    char *argv[128];
    int argc = 0;
    int code;

    luaL_checktype(L, 1, LUA_TTABLE);
    argv[argc++] = "curl";
    for (lua_Integer i = 1; argc < 127 && lua_rawgeti(L, 1, i) == LUA_TSTRING; i++) {
        argv[argc++] = (char *)lua_tostring(L, -1);     /* kept on the stack */
    }
    lua_pop(L, 1);
    argv[argc] = NULL;

    code = posix_program_run_to(curltest_curl_main, argc, argv, PROGRAM_STACK_PAGES, log);
    if (code < 0 && errno == ENOMEM) {
        return luaL_error(L, "curltest: no room for curl's stack");
    }
    if (code < 0) {
        return luaL_error(L, "curltest: %s: %s", log, strerror(errno));
    }
    lua_pushinteger(L, code);
    return 1;
}

void kosmos_curltest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
}
