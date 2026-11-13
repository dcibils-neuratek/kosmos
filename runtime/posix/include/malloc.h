/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_MALLOC_H
#define POSIX_MALLOC_H

/*
 * **`<malloc.h>`**, as glibc and Android give it to ported code (Skia's
 * `sk_malloc_size`): the allocator's own functions, and how much of a
 * block can be used - at least what was asked for, rounded up to the
 * allocator's sixteen (`runtime/libc/malloc.c`).
 */

#include <stdlib.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

size_t malloc_usable_size(void *p);

__END_DECLS

#endif /* POSIX_MALLOC_H */
