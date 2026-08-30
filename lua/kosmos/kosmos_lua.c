/*
 * Everything Lua needs from Kosmos, and the decisions about what Lua is
 * allowed to reach.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "kosmos_lua.h"
#include "console.h"
#include "panic.h"

void kosmos_lua_write(const char *s, size_t len)
{
    /* Not kputs: Lua hands out lengths, and a Lua string may legitimately
     * contain a zero byte. */
    size_t i;

    for (i = 0; i < len; i++) {
        kputc(s[i]);
    }
}

void kosmos_lua_writeerror(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    kputs(buf);
}

long kosmos_lua_time(long *out)
{
    /* Not a wall clock, and not pretending to be one. See the header. */
    uint64_t counter;
    long value;

    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(counter));
    value = (long)counter;

    if (out != NULL) {
        *out = value;
    }

    return value;
}

unsigned int kosmos_lua_seed(void)
{
    /*
     * Randomising string hashes is a defence against an attacker who can
     * choose keys and wants every one of them to collide. Upstream seeds
     * from the wall clock and from addresses, relying on ASLR.
     *
     * Neither input exists here. There is no clock, and the kernel is
     * identity mapped at a fixed address, so upstream's seed would be
     * identical on every boot. The cycle counter is at least genuinely
     * different each time, which is what the seed needs.
     *
     * It is not a strong source and it does not need to be yet: nothing
     * untrusted reaches Lua until there are user processes at M4. When it
     * does, this should come from the same place other randomness does,
     * and that place does not exist yet.
     */
    uint64_t counter;

    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(counter));

    return (unsigned int)(counter ^ (counter >> 32));
}

/*
 * Lua's allocator.
 *
 * The contract is one function for all four operations: nsize of zero is a
 * free, a NULL pointer is an allocation, and anything else is a resize.
 * Returning NULL from a resize has to leave the original block untouched,
 * which is what lets Lua raise an out-of-memory error instead of losing the
 * object it was growing.
 */
static void *kosmos_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
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
 * Which libraries exist inside a Lua state.
 *
 * `design.md` §5.3: this list is part of the security model, not a
 * configuration detail. Upstream's linit.c opens everything; this replaces
 * it, and the omissions are the point.
 *
 * Out:
 *
 *   io   `io.open("/etc/passwd")` is semantically incoherent in Kosmos.
 *        There is no global tree to open a path in. Files are reached
 *        through the process's namespace, and that is `fs`, at M5.
 *   os   Wall clock, environment variables, `os.execute`, `os.tmpname`.
 *        Every one of them assumes a Unix underneath.
 *   package / require with dynamic loading, which wants dlopen. Kosmos has
 *        no dynamic linker. Our own `require` resolves against the
 *        namespace, at M5.
 *   debug `debug.getupvalue` and `debug.setmetatable` break any abstraction
 *        built in Lua, including the ones the capability model will rest on.
 *        It is a debugging tool that is also a hole, and it stays out until
 *        there is a reason to hand it to a specific process.
 */
static const luaL_Reg kosmos_libs[] = {
    { LUA_GNAME,      luaopen_base    },
    { LUA_COLIBNAME,  luaopen_coroutine },
    { LUA_TABLIBNAME, luaopen_table   },
    { LUA_STRLIBNAME, luaopen_string  },
    { LUA_MATHLIBNAME, luaopen_math   },
    { LUA_UTF8LIBNAME, luaopen_utf8   },
    { NULL, NULL }
};

/*
 * The base library carries two functions that do not belong here, and
 * leaving out liolib and loslib does not remove them: `dofile` and
 * `loadfile` are registered by luaopen_base itself.
 *
 * Both take a path and open it. There is no path to open: what a process can
 * reach is what was mounted into its namespace, and a bare filename means
 * nothing outside one. They would fail anyway, because every stream function
 * here fails, but a function that fails is a function that says a global tree
 * exists and happens to be empty. That is the assumption `design.md` §17.1
 * spends a section rejecting, and it is exactly how POSIX comes back in
 * through the side door.
 *
 * `load` stays. It takes source, not a path.
 */
static const char *const removed_globals[] = {
    "dofile",
    "loadfile",
    NULL
};

static void open_libs(lua_State *L)
{
    const luaL_Reg *lib;
    const char *const *name;

    for (lib = kosmos_libs; lib->func != NULL; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);  /* requiref leaves the module on the stack */
    }

    for (name = removed_globals; *name != NULL; name++) {
        lua_pushnil(L);
        lua_setglobal(L, *name);
    }
}

static int at_panic(lua_State *L)
{
    const char *msg = lua_tostring(L, -1);

    /*
     * Reached when an error escapes with no protected call anywhere below
     * it. Inside a process at M4 this kills that process and nothing else;
     * today Lua is in the kernel, so there is nothing to kill it apart from.
     */
    kputs("\nPANIC: unprotected error in Lua: ");
    kputs((msg != NULL) ? msg : "(no message)");
    kputs("\n");

    panic("Lua called its panic function");
}

lua_State *kosmos_lua_open(void)
{
    lua_State *L = lua_newstate(kosmos_alloc, NULL);

    if (L == NULL) {
        return NULL;
    }

    lua_atpanic(L, at_panic);
    open_libs(L);

    return L;
}

int kosmos_lua_dostring(lua_State *L, const char *chunkname, const char *src)
{
    int status;

    /*
     * Mode "t" and never "bt".
     *
     * `design.md` §5.3 forbids precompiled bytecode outright: the undump
     * loader validates almost nothing, and a crafted chunk is arbitrary
     * execution inside the state. Upstream's default is "bt", which accepts
     * either, so the restriction has to be stated at every load rather than
     * relied on.
     */
    status = luaL_loadbufferx(L, src, strlen(src), chunkname, "t");

    if (status == LUA_OK) {
        status = lua_pcall(L, 0, LUA_MULTRET, 0);
    }

    return status;
}
