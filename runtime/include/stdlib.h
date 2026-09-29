/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

/*
 * The heap here is not the kernel's.
 *
 * CLAUDE.md forbids a dynamic allocator in the kernel, and that stands: the
 * kernel's own state lives in fixed-size pools. This is Lua's heap, a region
 * of pages taken once from the page allocator and sub-divided inside itself,
 * which is what `design.md` §5.2 means by "one lua_State per process, with a
 * heap limit".
 *
 * Nothing in kernel/, arch/ or hal/ may call these. Lua may, because Lua
 * cannot be given whole pages: it allocates a table header at a time.
 */

/* Hands the heap the memory it manages. Called once, before Lua exists. */
void   heap_init(void *base, size_t size);
size_t heap_used(void);
size_t heap_size(void);

long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
int    atoi(const char *s);
double atof(const char *s);
char  *getenv(const char *name);
int    system(const char *command);

/*
 * A landing place for `exit()` (the note in malloc.c has why): arm it, then
 * `setjmp(kosmos_exit_to)` in the function that will still be running when
 * something calls `exit` - 0 the first time, and on arriving back because
 * something did, 1 for an `exit(0)` and 2 for any other.
 *
 *     kosmos_exit_arm();
 *
 *     if (setjmp(kosmos_exit_to) != 0) {
 *         ... it stopped, and said why ...
 *     }
 *
 * **Two lines, and they have to be.** This was one function that called
 * `setjmp` itself and returned, and a jump back into a function that has
 * returned is undefined: the landing arrived in whatever the next call had
 * left where its frame was. Doom's and Quake's `exit` at startup came back
 * as the call after the arming returning, so `l_start` said the engine had
 * *started*, and Quake drew frames of a half-made game until it said "load
 * failed." (29 September, `testing.md` 18.281). So `setjmp` is called in the
 * frame the jump comes back to, and as C allows it - the whole of an `if`'s
 * or a `switch`'s condition, never the right side of an assignment.
 */
#include <setjmp.h>

extern jmp_buf kosmos_exit_to;
void   kosmos_exit_arm(void);
void   kosmos_exit_disarm(void);

/*
 * `fn(arg)` on another stack, the one ending at `top`, and back. For a
 * vendored engine whose frames outgrow a process's stack; the caller owns
 * that memory and its guard page. See `callstack-aarch64.S`.
 */
void   kosmos_call_on_stack(void (*fn)(void *), void *arg, void *top);
void   qsort(void *base, size_t count, size_t size,
             int (*compare)(const void *, const void *));
void  *bsearch(const void *key, const void *base, size_t count, size_t size,
               int (*compare)(const void *, const void *));

/*
 * The standard pseudo-random pair.
 *
 * Here rather than in a port's shim because they are C, not SDL: the first
 * caller was Lite XL's `rencache.c`, which reaches them through `<SDL.h>`
 * the way every SDL program does, but the next one will reach them through
 * `<stdlib.h>` like everybody else.
 *
 * Not for anything that must not be guessed. `crypto.c` is where randomness
 * with a requirement on it lives; this is the one from the C standard, and
 * it is the C standard's own example generator.
 */
#define RAND_MAX 32767

int    rand(void);
void   srand(unsigned seed);

void  *malloc(size_t n);
void  *calloc(size_t count, size_t size);
void  *realloc(void *p, size_t n);
void   free(void *p);

/* Integer absolute value. Lua uses abs on line deltas in its debug info.
 * abs(INT_MIN) is undefined in C and stays undefined here rather than being
 * quietly given a wrong answer. */
static inline int       abs(int n)            { return n < 0 ? -n : n; }
static inline long      labs(long n)          { return n < 0 ? -n : n; }
static inline long long llabs(long long n)    { return n < 0 ? -n : n; }

void   abort(void) __attribute__((noreturn));
void   exit(int status) __attribute__((noreturn));

/* Lua parses its own integers; strtod is what it uses for float literals. */
double strtod(const char *s, char **end);

#define EXIT_SUCCESS    0
#define EXIT_FAILURE    1

#endif /* STDLIB_H */
