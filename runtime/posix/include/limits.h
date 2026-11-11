/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_LIMITS_H
#define POSIX_LIMITS_H

/* The compiler's, and the length of a path - the Storage Kit's, a whole
 * path as `/Home` takes it, with its terminating zero. */
#include_next <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 512
#endif

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#endif /* POSIX_LIMITS_H */
