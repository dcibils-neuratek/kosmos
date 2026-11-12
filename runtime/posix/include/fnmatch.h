/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_FNMATCH_H
#define POSIX_FNMATCH_H

/*
 * **A name held to a shell's pattern, for ported code** (`runtime/posix/glob.c`):
 * `*`, `?` and `[...]`, with POSIX's flags. Nothing here reads a disk.
 */

#include <sys/cdefs.h>

__BEGIN_DECLS

#define FNM_NOMATCH   1

#define FNM_NOESCAPE  0x01      /* a backslash is a character, not a quote */
#define FNM_PATHNAME  0x02      /* a slash is matched only by a slash */
#define FNM_PERIOD    0x04      /* a leading period only by a period */

int fnmatch(const char *pattern, const char *string, int flags);

__END_DECLS

#endif /* POSIX_FNMATCH_H */
