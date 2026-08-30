/*
 * The two functions the compiler generates calls to on its own.
 *
 * Even with -ffreestanding, GCC recognises a zeroing loop or a struct copy
 * and emits a call to memset or memcpy. With -nostdlib there is nothing to
 * link against, so they have to exist. Two functions, no header of their
 * own: nothing calls them by name.
 *
 * These are the kernel's, deliberately naive. The real freestanding libc for
 * userland is a separate thing and arrives at M2.
 */

#include <stddef.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    while (n-- > 0) {
        *d++ = (unsigned char)c;
    }

    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n-- > 0) {
        *d++ = *s++;
    }

    return dst;
}
