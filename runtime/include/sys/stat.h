/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SYS_STAT_H
#define SYS_STAT_H

#include <sys/cdefs.h>
__BEGIN_DECLS

/*
 * Enough of `<sys/stat.h>` for a vendored file that wants to make a
 * directory: `mkdir`, which fails (`user/init/misc_user.c`), so the caller
 * checks, says so, and goes on without it - pretending to succeed would
 * lose whatever was to be written there.
 *
 * Kosmos's own code asks about a file through the namespace, from Lua.
 * **Ported code has a `stat` and a working `mkdir`** in `runtime/posix/`,
 * whose `sys/stat.h` shadows this one for what links it, on the Storage
 * Kit and in the process's own namespace (`docs/w3.md` W3d2); the `mkdir`
 * here is weak so that one wins there. This said there was no `stat` "and
 * there is not going to be one" until the principle was amended on 11
 * November 2026.
 */

#include <sys/types.h>

int mkdir(const char *path, mode_t mode);

__END_DECLS

#endif /* SYS_STAT_H */
