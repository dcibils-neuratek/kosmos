/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **`posix_memalign`**, for ported code that asks for memory on a wide
 * boundary the POSIX way rather than C11's: FFmpeg, whose assembly needs
 * frames on 32 or 64 bytes and whose `configure` refuses its assembly
 * without it (`docs/media.md` M1). It is `aligned_alloc` underneath
 * (`runtime/libc/malloc.c`), so `free` takes what it gives.
 *
 * What POSIX asks of it, and C11's does not: the boundary a power of two
 * and a multiple of a pointer's size, the answer an error number rather
 * than `errno`, and `*out` untouched when it fails.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

int posix_memalign(void **out, size_t align, size_t n)
{
    void *p;

    if (align == 0 || (align & (align - 1)) != 0 || align % sizeof(void *) != 0) {
        return EINVAL;
    }

    p = aligned_alloc(align, n);
    if (p == NULL) {
        return ENOMEM;
    }

    *out = p;
    return 0;
}
