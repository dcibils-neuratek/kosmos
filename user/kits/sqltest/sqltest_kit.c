/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to W5c's SQLite test (`docs/w5.md`): a kit in no image but
 * `sqltest.elf`, reached as `use("sqltest.elf")`.
 *
 * - `fuzzcheck(args)` and `speedtest1(args)` run SQLite's own programs
 *   with that argument list (`posix_program.h`), and return what their
 *   `main` returned and what they printed, for the program to print.
 * - `locks(path)` and `keep(path, "write" | "read")` return the line
 *   `sqltest.c` says, for the program to print; `exec(path, sql)` runs
 *   SQL on a database and says how it went, with SQLite's extended code
 *   and `errno` when it went wrong.
 */

#include <errno.h>
#include <posix_program.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kits/kitenv.h"
#include "sqltest.h"

/*
 * **The programs' stack**: `fuzzcheck` ran off a process's 256 KB inside
 * SQLite's query planner (`whereScanNext`) on the second fuzz database. On
 * this Mac it needs between 512 KB and a megabyte - it fails at
 * `ulimit -s 512` and passes at 1024 - so it is given eight, what the
 * programs assume of a desktop. `posix_program_run` maps it, and catches
 * their `exit` (`-Dexit=posix_program_exit`, the Makefile).
 */
#define PROGRAM_STACK_PAGES 2048

static int run_main(lua_State *L, int (*entry)(int, char **), const char *name)
{
    char *argv[64];
    int argc = 0;
    int code;

    luaL_checktype(L, 1, LUA_TTABLE);
    argv[argc++] = (char *)name;
    for (lua_Integer i = 1; argc < 63 && lua_rawgeti(L, 1, i) == LUA_TSTRING; i++) {
        argv[argc++] = (char *)lua_tostring(L, -1);     /* kept on the stack */
    }
    lua_pop(L, 1);
    argv[argc] = NULL;

    sqltest_fresh(entry != sqltest_fuzzcheck_main);

    if ((code = posix_program_run(entry, argc, argv, PROGRAM_STACK_PAGES)) < 0
        && errno == ENOMEM) {
        return luaL_error(L, "%s: no room for its stack", name);
    }

    lua_pushinteger(L, code);
    kit_push_spill(L);
    return 2;
}

static int l_fuzzcheck(lua_State *L)
{
    return run_main(L, sqltest_fuzzcheck_main, "fuzzcheck");
}

static int l_speedtest1(lua_State *L)
{
    return run_main(L, sqltest_speedtest1_main, "speedtest1");
}

static void keep_line(const char *line, void *ctx)
{
    lua_pushstring((lua_State *)ctx, line);
}

static int l_locks(lua_State *L)
{
    lua_pushnil(L);
    sqltest_fresh(1);
    sqltest_locks(luaL_checkstring(L, 1), keep_line, L);
    return 1;
}

static int l_keep(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    static const char *const modes[] = { "read", "write", NULL };
    int write = luaL_checkoption(L, 2, NULL, modes);

    lua_pushnil(L);
    sqltest_fresh(1);
    sqltest_keep(path, write, keep_line, L);
    return 1;
}

static int l_exec(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    const char *sql = luaL_checkstring(L, 2);

    lua_pushnil(L);
    sqltest_fresh(1);
    sqltest_exec(path, sql, keep_line, L);
    return 1;
}

void kosmos_sqltest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_fuzzcheck);
    lua_setfield(L, -2, "fuzzcheck");
    lua_pushcfunction(L, l_speedtest1);
    lua_setfield(L, -2, "speedtest1");
    lua_pushcfunction(L, l_locks);
    lua_setfield(L, -2, "locks");
    lua_pushcfunction(L, l_keep);
    lua_setfield(L, -2, "keep");
    lua_pushcfunction(L, l_exec);
    lua_setfield(L, -2, "exec");
}
