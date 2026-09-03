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
int    atoi(const char *s);
char  *getenv(const char *name);
void   qsort(void *base, size_t count, size_t size,
             int (*compare)(const void *, const void *));

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
