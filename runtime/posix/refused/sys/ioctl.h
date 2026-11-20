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

/*
 * **A request's number, as Linux encodes it** - direction, size, type and
 * number in one word - for a port whose headers name requests: Mesa's DRM
 * headers, which declare i915's and Xe's whole interface as constants
 * (`docs/gpu3d.md` G7a). Arithmetic, and no door: the numbers are only
 * ever handed to the `ioctl` below, which refuses them.
 */
#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U
#define _IOC(dir, type, nr, size) \
    (((unsigned long)(dir) << 30) | ((unsigned long)(size) << 16) | \
     ((unsigned long)(type) << 8) | (unsigned long)(nr))
#define _IO(type, nr)         _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, t)     _IOC(_IOC_READ, (type), (nr), sizeof(t))
#define _IOW(type, nr, t)     _IOC(_IOC_WRITE, (type), (nr), sizeof(t))
#define _IOWR(type, nr, t)    _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(t))
#define _IOC_NR(nr)           ((nr) & 0xff)
#define _IOC_TYPE(nr)         (((nr) >> 8) & 0xff)
#define _IOC_SIZE(nr)         (((nr) >> 16) & 0x3fff)
#define _IOC_DIR(nr)          (((nr) >> 30) & 0x3)

static inline int ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    (void)request;
    errno = ENOTTY;
    return -1;
}

#endif
