/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_STDLIB_H
#define POSIX_STDLIB_H

/* The runtime's, and the POSIX calls that live in `<stdlib.h>`. */
#include_next <stdlib.h>

#include <sys/cdefs.h>

__BEGIN_DECLS

/* `path` made whole and found: into `resolved` (PATH_MAX bytes), or a new
 * allocation when it is NULL. */
char *realpath(const char *path, char *resolved);

/* A new file or folder named from `pattern`, whose last six `X`s are
 * replaced (`stdio.c`): a descriptor, or `pattern` itself. */
int   mkstemp(char *pattern);
int   mkostemp(char *pattern, int flags);   /* O_CLOEXEC, O_APPEND */
char *mkdtemp(char *pattern);

/* **There is no environment** (`getenv`, `user/init/misc_user.c`), so
 * nothing can be put into one: both refuse with ENOTSUP, as `flock` does,
 * and a port finds out rather than believing a later `getenv` will answer
 * (`process.c`). */
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);

__END_DECLS

#endif /* POSIX_STDLIB_H */
