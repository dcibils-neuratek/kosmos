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

void  *malloc(size_t n);
void  *calloc(size_t count, size_t size);
void  *realloc(void *p, size_t n);
void   free(void *p);

void   abort(void) __attribute__((noreturn));
void   exit(int status) __attribute__((noreturn));

/* Lua parses its own integers; strtod is what it uses for float literals. */
double strtod(const char *s, char **end);

#define EXIT_SUCCESS    0
#define EXIT_FAILURE    1

#endif /* STDLIB_H */
