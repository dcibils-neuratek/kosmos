/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_FCNTL_H
#define POSIX_FCNTL_H

/*
 * **`open`, for ported code** (`docs/w3.md` W3d2), in place of the
 * runtime's empty `fcntl.h`. A descriptor is private to this process and
 * names a path in its own namespace, through the Storage Kit; nothing
 * crosses to another process, and there is no `exec` to inherit one.
 *
 * Linux's generic numbers, so a port's own table of them agrees.
 */

#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

#define O_RDONLY    00
#define O_WRONLY    01
#define O_RDWR      02
#define O_ACCMODE   03
#define O_CREAT     0100
#define O_EXCL      0200
#define O_NOCTTY    0400        /* accepted; there are no terminals to take */
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000       /* accepted; a file never makes one wait long */
#define O_NDELAY    O_NONBLOCK
#define O_SYNC      04010000    /* accepted */
#define O_DSYNC     010000      /* accepted */
#define O_DIRECTORY 0200000
#define O_NOFOLLOW  0400000     /* accepted; there are no links */
#define O_CLOEXEC   02000000    /* accepted; there is no exec */

#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_DUPFD_CLOEXEC  1030

#define FD_CLOEXEC 1

#define AT_FDCWD   -100

int open(const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int command, ...);

__END_DECLS

#endif /* POSIX_FCNTL_H */
