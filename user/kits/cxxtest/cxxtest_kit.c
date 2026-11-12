/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to `cxxtest.cpp` (`docs/webkit.md` W1d): a kit in no image but
 * `cxxtest.elf`, reached as `use("cxxtest.elf")`, as `apptest` is.
 *
 * `run()` is the C++'s own sentence; `threads()` counts its `thread_local`
 * on this thread and a second one - "main 8, thread 105, main again 8":
 * three added here, a hundred there, and neither seeing the other's.
 */

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"
#include "tls.h"

int kosmos_cxxtest_run(char *out, unsigned long size);
int kosmos_cxxtest_count(int times);

int kosmos_cxxtest_std(char *out, unsigned long size);

static int l_std(lua_State *L)
{
    char said[200];

    (void)kosmos_cxxtest_std(said, sizeof(said));
    lua_pushstring(L, said);
    return 1;
}

static int l_run(lua_State *L)
{
    char said[160];

    (void)kosmos_cxxtest_run(said, sizeof(said));
    lua_pushstring(L, said);
    return 1;
}

/* The second thread: its copy of the variables made here, in a function
 * that uses none of them, before the C++ that does (`tls.h`). */
static void second(unsigned long arg)
{
    void *made = kosmos_tls_begin();
    int saw = made != NULL ? kosmos_cxxtest_count((int)arg) : -1;

    kosmos_tls_end(made);
    kosmos_thread_exit(saw);
}

static int l_threads(lua_State *L)
{
    int main_count = kosmos_cxxtest_count(3);
    long t = kosmos_thread_start(second, 100);
    long code = t < 0 ? t : kosmos_thread_wait((unsigned long)t);

    lua_pushfstring(L, "main %d, thread %d, main again %d",
                    main_count, (int)code, kosmos_cxxtest_count(0));
    return 1;
}

void kosmos_cxxtest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
    lua_pushcfunction(L, l_threads);
    lua_setfield(L, -2, "threads");
    lua_pushcfunction(L, l_std);
    lua_setfield(L, -2, "std");
}
