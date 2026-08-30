#ifndef STRING_H
#define STRING_H

#include <stddef.h>

/*
 * The freestanding C library, such as it is.
 *
 * Two audiences share it for now, because at M2 there is only one address
 * space: the kernel, and Lua compiled into the same image. `design.md` §17.3
 * lists what Lua needs and this is that list, nothing more. Functions get
 * added when a link error asks for one, never in anticipation.
 *
 * At M4, when Lua moves to EL0, the kernel takes its own copy back and this
 * becomes what the design calls it: the libc that lives inside a process,
 * whose I/O resolves against that process's namespace and nowhere else.
 *
 * memcpy and memset are not optional even if nothing calls them by name.
 * GCC recognises a zeroing loop or a struct copy and emits a call, and
 * -nostdlib leaves nothing to link against.
 */

void   *memcpy(void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
void   *memset(void *dst, int c, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
void   *memchr(const void *s, int c, size_t n);

size_t  strlen(const char *s);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strcpy(char *dst, const char *src);
char   *strchr(const char *s, int c);

#endif /* STRING_H */
