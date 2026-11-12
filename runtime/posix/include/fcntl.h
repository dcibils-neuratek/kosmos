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

/*
 * **Record locks, named and refused** (`docs/w5.md` W5c), as `flock` is
 * (`sys/file.h`): the servers keep no locks, and one this layer kept would
 * bind only this process. SQLite compiles its POSIX locking against these
 * names whichever way it locks, and on Kosmos it locks with a directory
 * made beside the database instead (`unix-dotfile`), which the server
 * makes or refuses atomically. F_GETLK, F_SETLK and F_SETLKW say ENOTSUP.
 */
#define F_GETLK          5
#define F_SETLK          6
#define F_SETLKW         7

#define F_RDLCK          0
#define F_WRLCK          1
#define F_UNLCK          2

struct flock {
    short l_type;
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};

#define FD_CLOEXEC 1

#define AT_FDCWD   -100
#define AT_SYMLINK_NOFOLLOW 0x100   /* accepted; there are no links */
#define AT_REMOVEDIR        0x200

int open(const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int command, ...);

/* `path` relative to the folder `dirfd` opened, or to the working
 * directory for AT_FDCWD (`fd.c`). */
int openat(int dirfd, const char *path, int flags, ...);

__END_DECLS

#endif /* POSIX_FCNTL_H */
