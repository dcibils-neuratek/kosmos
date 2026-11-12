/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_GLOB_H
#define POSIX_GLOB_H

/*
 * **The names a pattern matches, for ported code** (`runtime/posix/glob.c`):
 * a wildcard in any part of the path, read through `readdir`, so through
 * the process's own namespace like every other path here. Sorted unless
 * asked not to be. No tilde, no braces: those are a shell's, not POSIX's.
 */

#include <sys/cdefs.h>
#include <stddef.h>

__BEGIN_DECLS

typedef struct {
    size_t gl_pathc;            /* names matched */
    char **gl_pathv;            /* them, then a NULL */
    size_t gl_offs;             /* NULLs before them, with GLOB_DOOFFS */
} glob_t;

#define GLOB_ERR      0x01      /* stop at a folder that cannot be read */
#define GLOB_MARK     0x02      /* a slash after each folder's name */
#define GLOB_NOSORT   0x04
#define GLOB_DOOFFS   0x08
#define GLOB_NOCHECK  0x10      /* no match: the pattern itself */
#define GLOB_APPEND   0x20
#define GLOB_NOESCAPE 0x40

#define GLOB_NOSPACE  1
#define GLOB_ABORTED  2
#define GLOB_NOMATCH  3

int  glob(const char *pattern, int flags,
          int (*errfunc)(const char *path, int error), glob_t *found);
void globfree(glob_t *found);

__END_DECLS

#endif /* POSIX_GLOB_H */
