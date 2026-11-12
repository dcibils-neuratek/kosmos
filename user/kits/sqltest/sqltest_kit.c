/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to W5c's SQLite test (`docs/w5.md`): a kit in no image but
 * `sqltest.elf`, reached as `use("sqltest.elf")`.
 *
 * - `fuzzcheck(args)` and `speedtest1(args)` run SQLite's own programs
 *   with that argument list, and return what their `main` returned and
 *   what they printed. A program at the prompt does not own the console,
 *   so what they print waits in the spill ring (`kosmos.h`) until the call
 *   is over, and comes back as the second value for the program to print.
 * - `locks(path)` and `keep(path, "write" | "read")` return the line
 *   `sqltest.c` says, for the program to print; `exec(path, sql)` runs
 *   SQL on a database and says how it went, with SQLite's extended code
 *   and `errno` when it went wrong.
 */

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "kosmos.h"
#include "lua.h"
#include "lauxlib.h"

#include "sqltest.h"

/*
 * **`exit`, as the programs call it** (`-Dexit=sqltest_exit`, the
 * Makefile): back to `run_main` with the code. Kosmos's own `exit` ends
 * the process, and `fuzzcheck` calls it on any failure after saying why -
 * which then went with the process. Nothing the programs left open is
 * closed; a run is one call.
 */
static jmp_buf leaving;
static int     left_with;

void sqltest_exit(int code)
{
    left_with = code;
    longjmp(leaving, 1);
}

/*
 * **The programs' own stack**, as Quake's engine has one
 * (`quake_kosmos.c`): a process's stack is 256 KB, and `fuzzcheck` ran off
 * it inside SQLite's query planner (`whereScanNext`) on the second fuzz
 * database. On this Mac it needs between 512 KB and a megabyte - it fails
 * at `ulimit -s 512` and passes at 1024 - so it is given eight, what the
 * programs assume of a desktop. One page more is mapped and the lowest
 * unmapped again, a guard an overflow faults on.
 */
#define PROGRAM_STACK_PAGES 2048

static void *program_stack_top;

struct run {
    int (*entry)(int, char **);
    int    argc;
    char **argv;
    int    code;
};

/* The program, or the code it left with - on the programs' stack, so the
 * jump back is within it. */
static void call(void *arg)
{
    struct run *r = arg;

    if (setjmp(leaving) != 0) {
        r->code = left_with;
        return;
    }

    r->code = r->entry(r->argc, r->argv);
}

static int run_main(lua_State *L, int (*entry)(int, char **), const char *name)
{
    char *argv[64];
    int argc = 0;

    luaL_checktype(L, 1, LUA_TTABLE);
    argv[argc++] = (char *)name;
    for (lua_Integer i = 1; argc < 63 && lua_rawgeti(L, 1, i) == LUA_TSTRING; i++) {
        argv[argc++] = (char *)lua_tostring(L, -1);     /* kept on the stack */
    }
    lua_pop(L, 1);
    argv[argc] = NULL;

    if (program_stack_top == NULL) {
        long base = kosmos_map(PROGRAM_STACK_PAGES + 1);

        if (base < 0 || kosmos_unmap((unsigned long)base, 1) < 0) {
            return luaL_error(L, "%s: no room for its stack", name);
        }
        program_stack_top = (void *)(uintptr_t)((unsigned long)base
                            + (PROGRAM_STACK_PAGES + 1) * KOSMOS_PAGE_SIZE);
    }

    struct run r = { entry, argc, argv, 0 };

    sqltest_fresh(entry != sqltest_fuzzcheck_main);

    kosmos_call_on_stack(call, &r, program_stack_top);
    int code = r.code;
    fflush(stdout);
    lua_pushinteger(L, code);

    luaL_Buffer said;
    char chunk[4096];
    size_t n;

    luaL_buffinit(L, &said);
    while ((n = kosmos_spill_drain(chunk, sizeof(chunk))) > 0) {
        luaL_addlstring(&said, chunk, n);
    }
    luaL_pushresult(&said);
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
