/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_SYS_FILE_H
#define POSIX_SYS_FILE_H

/*
 * **`flock`, named and refused** (`docs/w4.md` W4c). A file is its
 * server's, reached by every process that names it, and the servers keep
 * no locks; a lock this layer kept would bind only this process, which
 * is no lock at all. So `flock` finds the descriptor and says ENOTSUP,
 * and a port learns that it holds nothing (`fd.c`).
 */

#include <sys/cdefs.h>

__BEGIN_DECLS

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

int flock(int fd, int operation);

__END_DECLS

#endif /* POSIX_SYS_FILE_H */
