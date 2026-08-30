#ifndef KOSMOS_LUA_H
#define KOSMOS_LUA_H

/*
 * How Lua is configured for Kosmos, without editing a line of it.
 *
 * This is a header for the *user* build and nothing else. The kernel had a
 * copy of Lua once and does not any more, so `lua/kosmos/` is down to this
 * file and the serialiser; the functions declared below are implemented in
 * `user/lib/lua_glue.c`.
 *
 * Forced in front of every Lua translation unit with -include, so these
 * definitions land before luaconf.h and lauxlib.h get to supply their own.
 * Every hook below is guarded upstream by #if !defined, which is what makes
 * this possible: `lua/upstream/` stays byte-for-byte what lua.org ships, and
 * `lua/patches/` stays empty. When 5.4.9 comes out it is a directory swap.
 *
 * Not defined here, and deliberately: LUA_USE_POSIX. Leaving it out is what
 * makes Lua fall back to plain setjmp/longjmp and jmp_buf for LUAI_THROW,
 * which is exactly what runtime/libc/setjmp.S implements. Defining it would
 * ask for _setjmp/_longjmp, which are a POSIX extension this system has no
 * business having.
 */

#include <stddef.h>

/* Where Lua's output goes. Not a stream: the console, directly. */
void kosmos_lua_write(const char *s, size_t len);

/* Lua's error path formats through this. Varargs because the upstream
 * expansion is fprintf(stderr, s, p) and p is not always a string. */
void kosmos_lua_writeerror(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* Seeds Lua's string hash randomisation. */
unsigned int kosmos_lua_seed(void);

#define lua_writestring(s, l)       kosmos_lua_write((s), (size_t)(l))
#define lua_writeline()             kosmos_lua_write("\n", 1)
#define lua_writestringerror(s, p)  kosmos_lua_writeerror((s), (p))

/*
 * Upstream seeds from time(NULL) and the addresses of a few objects, relying
 * on address space layout randomisation. Neither exists here: there is no
 * wall clock and the kernel is identity mapped at a fixed address, so both
 * inputs would be the same on every boot.
 */
#define luai_makeseed(L)            (kosmos_lua_seed())

/*
 * lmathlib.c seeds its random generator with `time(NULL)` directly, with no
 * hook to override, so this redirects the call itself.
 *
 * The system's own `time()` panics, and should: there is no wall clock
 * anywhere, and returning a plausible wrong number is worse than stopping.
 * But Lua is not asking what time it is, it is asking for something that
 * differs between states, and the monotonic counter answers that honestly.
 *
 * The redirection applies only to Lua's translation units, because this
 * header is forced in front of those and nothing else. Anything outside Lua
 * that asks for the time still gets told to read /dev/clock.
 */
long kosmos_lua_time(long *out);

#define time(t)     kosmos_lua_time(t)

/*
 * `ltablib.c` randomises its quicksort pivot for arrays over a hundred
 * elements, and builds the randomness out of `clock()` and `time()`. It is
 * guarded, so it can be replaced rather than having `clock()` exist to
 * satisfy it.
 *
 * That guard was found by the user build failing to link. It was already a
 * latent panic in the kernel's copy: `clock()` there stops the machine, so
 * sorting a table of more than a hundred elements would have taken the
 * system down the first time anyone did it. A link error on one side found a
 * run-time fault on the other, which is the argument for building the same
 * code two ways.
 */
#define l_randomizePivot()  (kosmos_lua_seed())

/*
 * There is one locale and it is C. Upstream reads localeconv()->decimal_point
 * on every number parsed; this makes it a constant the compiler folds away,
 * and means locale.h is never actually called into.
 */
#define lua_getlocaledecpoint()     '.'

/*
 * Bringing a state up.
 *
 * `struct lua_State` is forward declared rather than included, because this
 * header is forced in front of every Lua translation unit and cannot include
 * lua.h from there. The tag is the same one lua.h typedefs, so the two
 * declarations agree.
 */
struct lua_State;

/* A fresh state with its allocator on the heap and the permitted libraries
 * open. NULL when the heap could not satisfy it. */
struct lua_State *kosmos_lua_open(void);

/* Loads and runs a chunk of source, text only. Returns a LUA_* status. */
int kosmos_lua_dostring(struct lua_State *L, const char *chunkname,
                        const char *src);

/* "Lua 5.4.8". A function rather than the LUA_RELEASE macro, so a caller can
 * print it without including lua.h and inheriting all of Lua's configuration
 * along with it. */
const char *kosmos_lua_version(void);

#endif /* KOSMOS_LUA_H */
