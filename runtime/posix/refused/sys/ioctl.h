/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_REFUSED_SYS_IOCTL_H
#define POSIX_REFUSED_SYS_IOCTL_H

/*
 * **`ioctl`, refused** (`docs/w5.md` W5c and W5d). It is one of the names
 * Kosmos does not have even for ported code (CLAUDE.md, *Compatibility
 * inside a process*): a device's private verbs, reached by number, with
 * no shape a server could hold a caller to. So the POSIX layer does not
 * have this header. It is here, in `runtime/posix/refused/`, put on the
 * include path of a port that includes it - SQLite's Unix layer and
 * OpenSSL's sockets, whatever they are built for - and nowhere else.
 *
 * What it gives is a private `ioctl` that refuses, ENOTTY, as a device
 * does a request it does not know: `static`, so no image has a symbol of
 * that name, and a port that calls it learns at the call. SQLite calls it
 * only on Linux, for F2FS; OpenSSL only for FIONBIO, which is not defined
 * here, so it sets a socket non-blocking with `fcntl`.
 */

#include <errno.h>

static inline int ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    (void)request;
    errno = ENOTTY;
    return -1;
}

#endif
