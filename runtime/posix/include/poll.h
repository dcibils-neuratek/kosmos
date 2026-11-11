/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POLL_H
#define POLL_H

/*
 * `poll` over any of this process's descriptors (`docs/w3.md` W3e): a
 * socket waits in the stack (`NET_OP_POLL`), a file and the console are
 * always ready, as a disk file is on Linux.
 */

#include <sys/cdefs.h>

__BEGIN_DECLS

typedef unsigned long nfds_t;

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#define POLLIN     0x001
#define POLLPRI    0x002
#define POLLOUT    0x004
#define POLLERR    0x008
#define POLLHUP    0x010
#define POLLNVAL   0x020
#define POLLRDNORM 0x040
#define POLLRDBAND 0x080
#define POLLWRNORM 0x100
#define POLLWRBAND 0x200
#define POLLRDHUP  0x2000

int poll(struct pollfd *set, nfds_t count, int timeout_ms);

__END_DECLS

#endif /* POLL_H */
