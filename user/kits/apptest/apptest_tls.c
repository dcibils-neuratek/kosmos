/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **What C++ will ask of an image, asked in C first** (`docs/webkit.md`
 * W1b): a constructor the runtime ran before `main` asked the image for
 * anything, and a variable each thread has its own copy of. In the loader's
 * test image alone, so the system's carries neither and pays nothing;
 * `tools/run_loader.py` reads the sentence.
 */

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"
#include "tls.h"

static int constructed;

__attribute__((constructor))
static void construct(void)
{
    constructed = 7;
}

static _Thread_local int mine = 5;      /* .tdata: begins as 5 */
static _Thread_local int zeroed;        /* .tbss: begins as 0 */

/*
 * The second thread's work, in a function of its own. **A thread's copy is
 * made before any function that uses one begins** - here by `second`,
 * below - because the compiler takes the thread pointer to be one value for
 * a whole function: it reads the register once and keeps it, so a function
 * that changed the pointer halfway would go on reading the old one. That
 * is what the first version of this did, and it read 99 from somewhere that
 * was not its copy. `pthread_create`'s threads begin the same way
 * (`runtime/posix/pthread.c`, W3).
 */
__attribute__((noinline))
static int second_work(void)
{
    int saw = mine * 100 + zeroed;      /* the template's: 500 */

    mine = 99;
    zeroed = 1;
    return saw * 1000 + mine;           /* then its own: 500099 */
}

static void second(unsigned long arg)
{
    void *made = kosmos_tls_begin();
    int saw = made != NULL ? second_work() : -1;

    (void)arg;
    kosmos_tls_end(made);
    kosmos_thread_exit(saw);
}

/*
 * "constructed 7, main 6, thread 500099, main again 6": the first thread's
 * copy set to 6 before the second began, the second's own copy beginning as
 * the template, and the second's 99 never reaching the first.
 */
int kosmos_apptest_tls(lua_State *L)
{
    long t, code;

    mine = 6;
    t = kosmos_thread_start(second, 0);
    code = t < 0 ? t : kosmos_thread_wait((unsigned long)t);
    lua_pushfstring(L, "constructed %d, main %d, thread %d, main again %d",
                    constructed, 6, (int)code, mine);
    return 1;
}
