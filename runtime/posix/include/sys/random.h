/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SYS_RANDOM_H
#define SYS_RANDOM_H

/*
 * `getrandom` and `getentropy`, for ported code (`docs/w3.md` W3c), on the
 * runtime's `arc4random_buf` - the kernel's entropy, held to its health
 * test (`kernel/entropy.c`). The flags are accepted and change nothing:
 * there is no pool to wait for, and the bytes are never weaker.
 */

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

#define GRND_NONBLOCK 0x1
#define GRND_RANDOM   0x2

ssize_t getrandom(void *out, size_t bytes, unsigned flags);
int     getentropy(void *out, size_t bytes);    /* at most 256, as everywhere */

__END_DECLS

#endif /* SYS_RANDOM_H */
