/*
 * Lua's environment inside a process.
 *
 * The same job `lua/kosmos/kosmos_lua.c` does for the kernel's copy, done on
 * the other side of the boundary. It is separate rather than shared because
 * almost nothing carries across: output is a syscall rather than a UART
 * register, the allocator is this process's own heap rather than a shared
 * one, and there is no panic() to fall back on because a process that cannot
 * continue exits instead of stopping the machine.
 *
 * The kernel's copy is temporary. Once the REPL moves out here there is no
 * reason for Lua to be in the kernel at all, which is what `roadmap.md` M4
 * means by removing it.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "kosmos_lua.h"
#include "kosmos.h"

void kosmos_lua_write(const char *s, size_t len)
{
    (void)kosmos_write(s, len);
}

void kosmos_lua_writeerror(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0) {
        (void)kosmos_write(buf, (size_t)((n < (int)sizeof(buf))
                                         ? n : (int)sizeof(buf) - 1));
    }
}

long kosmos_lua_time(long *out)
{
    /*
     * A process has no clock and cannot read the counter: CNTPCT_EL0 is
     * readable from EL0 only if the kernel enables it, and it deliberately
     * has not. Lua wants this for hash seeding, so it gets an address off
     * its own stack, which differs between processes and boots for the same
     * reason ASLR would have given upstream something.
     *
     * It is weak, and the honest fix is a syscall that hands out randomness
     * from wherever the system eventually gets it. Nothing untrusted reaches
     * a Lua state yet, so it is not urgent, but it is not good either.
     */
    long here = (long)(uintptr_t)&out;

    if (out != NULL) {
        *out = here;
    }

    return here;
}

unsigned int kosmos_lua_seed(void)
{
    return (unsigned int)kosmos_lua_time(NULL);
}

/*
 * Lua's allocator, on this process's own heap.
 *
 * `design.md` §5.2 wants exactly this: one lua_State per process with a
 * bounded heap, because a small heap collects fast and the maximum GC pause
 * is the number that decides whether the system stutters. The bound is the
 * heap the kernel mapped, and running into it is an out-of-memory error
 * inside this process rather than pressure on anything else.
 */
static void *user_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;

    if (nsize == 0) {
        free(ptr);
        return NULL;
    }

    return realloc(ptr, nsize);
}

/*
 * Which libraries exist in here.
 *
 * The same list the kernel's copy opens and for the same reasons
 * (`design.md` §5.3): no io or os, because there is no global tree and no
 * clock; no package, which wants dlopen; no debug, which breaks every
 * abstraction the capability model will rest on.
 *
 * At M5 this stops being a list in a C file and becomes what the process's
 * namespace contains, which is where the design puts it.
 */
static const luaL_Reg libs[] = {
    { LUA_GNAME,       luaopen_base      },
    { LUA_COLIBNAME,   luaopen_coroutine },
    { LUA_TABLIBNAME,  luaopen_table     },
    { LUA_STRLIBNAME,  luaopen_string    },
    { LUA_MATHLIBNAME, luaopen_math      },
    { LUA_UTF8LIBNAME, luaopen_utf8      },
    { NULL, NULL }
};

/* Registered by user/lib/sys_user.c. */
int luaopen_sys(lua_State *L);

static int at_panic(lua_State *L)
{
    const char *msg = lua_tostring(L, -1);

    kosmos_lua_write("lua panic: ", 11);
    if (msg != NULL) {
        kosmos_lua_write(msg, strlen(msg));
    }
    kosmos_lua_write("\n", 1);

    /*
     * The difference that matters. In the kernel this had to stop the
     * machine; here it ends one process, and everything else carries on.
     * That is what buying isolation with an address space actually bought.
     */
    kosmos_exit(70);
    return 0;
}

const char *kosmos_lua_version(void)
{
    return LUA_RELEASE;
}

lua_State *kosmos_lua_open(void)
{
    lua_State *L = lua_newstate(user_alloc, NULL);
    const luaL_Reg *lib;

    if (L == NULL) {
        return NULL;
    }

    lua_atpanic(L, at_panic);

    for (lib = libs; lib->func != NULL; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }

    luaL_requiref(L, "sys", luaopen_sys, 1);
    lua_pop(L, 1);

    /* dofile and loadfile come from luaopen_base and take a path. There is
     * no path to take: what a process reaches is what was mapped into it.
     * See the kernel's copy for the longer version of this argument. */
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");

    return L;
}

int kosmos_lua_dostring(lua_State *L, const char *chunkname, const char *src)
{
    int status = luaL_loadbufferx(L, src, strlen(src), chunkname, "t");

    if (status == LUA_OK) {
        status = lua_pcall(L, 0, LUA_MULTRET, 0);
    }

    return status;
}
