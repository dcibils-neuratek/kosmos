/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SYS_SELECT_H
#define SYS_SELECT_H

/*
 * `select`, for ported code that waits on a descriptor this way rather
 * than with `poll` - OpenSSL's `BIO_socket_wait` among it (`docs/w5.md`
 * W5d). It is `poll` underneath (`socket.c`): the sets are turned into a
 * `pollfd` list and back, so the two answer the same.
 */

#include <sys/cdefs.h>
#include <sys/time.h>

__BEGIN_DECLS

#define FD_SETSIZE 1024

typedef struct {
    unsigned long bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_WORD_(fd) ((unsigned)(fd) / (8 * sizeof(unsigned long)))
#define FD_BIT_(fd)  (1ul << ((unsigned)(fd) % (8 * sizeof(unsigned long))))
#define FD_ZERO(set)     __builtin_memset((set), 0, sizeof(fd_set))
#define FD_SET(fd, set)   ((set)->bits[FD_WORD_(fd)] |= FD_BIT_(fd))
#define FD_CLR(fd, set)   ((set)->bits[FD_WORD_(fd)] &= ~FD_BIT_(fd))
#define FD_ISSET(fd, set) (((set)->bits[FD_WORD_(fd)] & FD_BIT_(fd)) != 0)

int select(int count, fd_set *readable, fd_set *writable, fd_set *failed,
           struct timeval *timeout);

__END_DECLS

#endif /* SYS_SELECT_H */
