/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to W5c's libxml2 and libpsl test (`docs/w5.md`): a kit in no
 * image but `xmltest.elf`, reached as `use("xmltest.elf")`.
 *
 * - `runtest(args)` runs libxml2's own `runtest.c` with that argument
 *   list, in the folder the program was started in, which has to hold
 *   libxml2's `test/` and `result/`.
 * - `psl(name, args)` runs one of libpsl's five tests - `is-public`,
 *   `is-public-all`, `cookie-domain`, `is-public-builtin` or
 *   `registrable-domain` - reading the list and its tests from
 *   `/Home/apps/xml/psl` (the Makefile tells them so).
 *
 * Each returns what the program's `main` returned and what it printed,
 * for the program to print (`posix_program.h`).
 */

#include <errno.h>
#include <posix_program.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kits/kitenv.h"

/* The programs' `main`s, renamed as they are compiled (the Makefile). */
int xmltest_runtest_main(int argc, char **argv);
int xmltest_psl_is_public_main(int argc, const char *const *argv);
int xmltest_psl_is_public_all_main(int argc, const char *const *argv);
int xmltest_psl_cookie_main(int argc, const char *const *argv);
int xmltest_psl_builtin_main(int argc, const char *const *argv);
int xmltest_psl_registrable_main(int argc, const char *const *argv);

/*
 * **libpsl's tests' one helper**, `common.c`'s, which runs the test again
 * under valgrind through `system` when `TESTS_VALGRIND` is set. Nothing
 * is set here - there is no environment (`runtime/posix/process.c`) - so
 * it is never called, and `common.c`, which needs a shell, is not linked.
 */
int run_valgrind(const char *valgrind, const char *executable)
{
    (void)valgrind;
    (void)executable;
    return 1;
}

/* What a test program is given: libxml2's has recursion in its schema
 * checks and both are written for a desktop's stack, as SQLite's are. */
#define PROGRAM_STACK_PAGES 2048

static int run(lua_State *L, int (*entry)(int, char **), const char *name, int args)
{
    char *argv[64];
    int argc = 0;
    int code;

    luaL_checktype(L, args, LUA_TTABLE);
    argv[argc++] = (char *)name;
    for (lua_Integer i = 1; argc < 63 && lua_rawgeti(L, args, i) == LUA_TSTRING; i++) {
        argv[argc++] = (char *)lua_tostring(L, -1);     /* kept on the stack */
    }
    lua_pop(L, 1);
    argv[argc] = NULL;

    if ((code = posix_program_run(entry, argc, argv, PROGRAM_STACK_PAGES)) < 0
        && errno == ENOMEM) {
        return luaL_error(L, "%s: no room for its stack", name);
    }

    lua_pushinteger(L, code);
    kit_push_spill(L);
    return 2;
}

static int l_runtest(lua_State *L)
{
    return run(L, xmltest_runtest_main, "runtest", 1);
}

static const struct {
    const char *name;
    int (*entry)(int, const char *const *);
} psl_tests[] = {
    { "is-public",          xmltest_psl_is_public_main },
    { "is-public-all",      xmltest_psl_is_public_all_main },
    { "cookie-domain",      xmltest_psl_cookie_main },
    { "is-public-builtin",  xmltest_psl_builtin_main },
    { "registrable-domain", xmltest_psl_registrable_main },
};

static int l_psl(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);

    for (size_t i = 0; i < sizeof(psl_tests) / sizeof(psl_tests[0]); i++) {
        if (strcmp(name, psl_tests[i].name) == 0) {
            /* `const char *const *` and `char **` are the same words. */
            return run(L, (int (*)(int, char **))(void (*)(void))psl_tests[i].entry,
                       name, 2);
        }
    }

    return luaL_error(L, "psl: no test %s", name);
}

void kosmos_xmltest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_runtest);
    lua_setfield(L, -2, "runtest");
    lua_pushcfunction(L, l_psl);
    lua_setfield(L, -2, "psl");
}
