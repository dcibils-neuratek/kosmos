/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_STDLIB_H
#define POSIX_STDLIB_H

/* The runtime's, and the one POSIX call that lives in `<stdlib.h>`. */
#include_next <stdlib.h>

#include <sys/cdefs.h>

__BEGIN_DECLS

/* `path` made whole and found: into `resolved` (PATH_MAX bytes), or a new
 * allocation when it is NULL. */
char *realpath(const char *path, char *resolved);

__END_DECLS

#endif /* POSIX_STDLIB_H */
