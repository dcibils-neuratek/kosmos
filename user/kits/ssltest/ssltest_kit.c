/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to W5d's OpenSSL test (`docs/w5.md`): a kit in no image but
 * `ssltest.elf`, reached as `use("ssltest.elf")`.
 *
 *     code, summary = kit.run(name, args, log)
 *
 * runs one of OpenSSL's own test programs - `evp` (`evp_test`, every
 * algorithm against its vectors), `bn` (`bntest`, the big numbers) or `ssl`
 * (`sslapitest`, a client and a server talking TLS to each other in memory)
 * - with that argument list, and gives back what its `main` returned and
 * the lines that say how it went.
 *
 * **One run a process.** OpenSSL's test library keeps its list of tests in
 * a table that only grows (`test/testutil/driver.c`), so a second program
 * in the same process would run the first one's tests too; each is started
 * from the prompt by a script of its own (`tools/run_ssl.py`).
 *
 * **Where its words go.** These programs say a megabyte about what they are
 * doing, and the spill ring holds 64 KB (`kosmos.h`). So for the length of
 * the run the POSIX layer's `stdout` and `stderr` are a file, `log`, and
 * what comes back is that file's TAP lines - each test's `ok` or `not ok`
 * and each file's `Completed` count, four levels of indent deep at most -
 * and the first lines that report an error, for a failure to explain
 * itself.
 */

#include <errno.h>
#include <posix_program.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kits/kitenv.h"
#include "testutil.h"

/*
 * **Three programs, one test library.** Each defines `setup_tests`,
 * `cleanup_tests` and `test_get_options`, which the library calls; they are
 * renamed as each is compiled (the Makefile), and these three - the names
 * the library knows - pass the call to the program that is running.
 */
#define PROGRAM(p)                                                         \
    int p##_main(int argc, char **argv);                                   \
    int p##_setup_tests(void);                                             \
    void p##_cleanup_tests(void);                                          \
    const OPTIONS *p##_test_get_options(void);

PROGRAM(ssltest_evp)
PROGRAM(ssltest_bn)
PROGRAM(ssltest_ssl)

/* The test library's own `main`, renamed (the Makefile). */
int ssltest_testutil_main(int argc, char **argv);

static const struct program {
    const char *name;
    int (*setup)(void);
    void (*cleanup)(void);
    const OPTIONS *(*options)(void);
} programs[] = {
    { "evp", ssltest_evp_setup_tests, ssltest_evp_cleanup_tests, ssltest_evp_test_get_options },
    { "bn",  ssltest_bn_setup_tests,  ssltest_bn_cleanup_tests,  ssltest_bn_test_get_options },
    { "ssl", ssltest_ssl_setup_tests, ssltest_ssl_cleanup_tests, ssltest_ssl_test_get_options },
};

static const struct program *running;

int setup_tests(void)
{
    return running->setup();
}

void cleanup_tests(void)
{
    running->cleanup();
}

const OPTIONS *test_get_options(void)
{
    return running->options();
}

/* `sslapitest` runs handshakes on threads, and both sides' recursion
 * through the state machine was written for a desktop's stack. */
#define PROGRAM_STACK_PAGES 2048

/* At most this many lines that say ERROR or FAIL come back with the rest. */
#define ERROR_LINES 60

/* Whether a line of TAP is one of the ones that say how it went. */
static int result_line(const char *line, int *errors)
{
    size_t indent = strspn(line, " ");
    const char *at = line + indent;

    if (strncmp(at, "not ok ", 7) == 0) {
        return 1;
    }
    if (indent <= 4 && (strncmp(at, "ok ", 3) == 0 || strncmp(at, "# Completed", 11) == 0
                        || strncmp(at, "1..", 3) == 0)) {
        return 1;
    }
    if (*errors < ERROR_LINES && (strstr(at, "ERROR") != NULL || strstr(at, "FAIL") != NULL)) {
        (*errors)++;
        return 1;
    }
    return 0;
}

static int l_run(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const char *log = luaL_checkstring(L, 3);
    char *argv[128];
    int argc = 0;
    int code;
    FILE *out, *was_out = stdout, *was_err = stderr;
    char line[1024];
    int errors = 0;
    luaL_Buffer said;

    running = NULL;
    for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) {
        if (strcmp(name, programs[i].name) == 0) {
            running = &programs[i];
        }
    }
    if (running == NULL) {
        return luaL_error(L, "ssltest: no program %s", name);
    }

    luaL_checktype(L, 2, LUA_TTABLE);
    argv[argc++] = (char *)name;
    for (lua_Integer i = 1; argc < 127 && lua_rawgeti(L, 2, i) == LUA_TSTRING; i++) {
        argv[argc++] = (char *)lua_tostring(L, -1);     /* kept on the stack */
    }
    lua_pop(L, 1);
    argv[argc] = NULL;

    if ((out = fopen(log, "w")) == NULL) {
        return luaL_error(L, "ssltest: %s: %s", log, strerror(errno));
    }

    stdout = out;
    stderr = out;
    code = posix_program_run(ssltest_testutil_main, argc, argv, PROGRAM_STACK_PAGES);
    fflush(out);
    stdout = was_out;
    stderr = was_err;
    fclose(out);
    if (code < 0 && errno == ENOMEM) {
        return luaL_error(L, "ssltest: %s: no room for its stack", name);
    }

    lua_pushinteger(L, code);
    luaL_buffinit(L, &said);
    if ((out = fopen(log, "r")) != NULL) {
        while (fgets(line, sizeof(line), out) != NULL) {
            if (result_line(line, &errors)) {
                luaL_addstring(&said, line);
            }
        }
        fclose(out);
    }
    luaL_pushresult(&said);
    kit_push_spill(L);              /* anything that reached the ring anyway */
    return 3;
}

void kosmos_ssltest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
}
