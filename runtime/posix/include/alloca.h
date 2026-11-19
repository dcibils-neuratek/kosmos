/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_ALLOCA_H
#define POSIX_ALLOCA_H

/*
 * **`alloca`, for ported code** (`docs/gpu3d.md` G1): memory on the
 * caller's own stack, gone when it returns - the compiler's, as every
 * libc's is. Mesa's NIR keeps small arrays this way. A thread's stack is
 * the size it was made with and does not grow (`docs/threads.md`), so what
 * is asked for this way has to be small; Kosmos's own code does not use it.
 */
#include <stddef.h>

#define alloca(size) __builtin_alloca(size)

#endif /* POSIX_ALLOCA_H */
