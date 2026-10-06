/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `<sys/select.h>` for libsmb2, and for nothing else (`smb_port.h`).
 *
 * `libsmb2.h` includes this wherever a compiler finds one, for the type its
 * server's `select` loop declares - `smb2_serve_port`, which `smb_port.h`
 * answers "not here" - and a structure of hooks that names `fd_set`. Without
 * this file the ARM build took newlib's from the toolchain and the x86-64
 * one found none: one platform, two answers. So the platform gives its own:
 * the shapes, and no `select` at all.
 */
#ifndef KOSMOS_SMB_SYS_SELECT_H
#define KOSMOS_SMB_SYS_SELECT_H

#include <string.h>

#define FD_SETSIZE  1024

typedef struct {
    unsigned long bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define SMB_FD_WORD(fd)  ((unsigned)(fd) / (8u * sizeof(unsigned long)))
#define SMB_FD_BIT(fd)   (1ul << ((unsigned)(fd) % (8u * sizeof(unsigned long))))

#define FD_ZERO(set)       memset((set), 0, sizeof(fd_set))
#define FD_SET(fd, set)    ((set)->bits[SMB_FD_WORD(fd)] |= SMB_FD_BIT(fd))
#define FD_CLR(fd, set)    ((set)->bits[SMB_FD_WORD(fd)] &= ~SMB_FD_BIT(fd))
#define FD_ISSET(fd, set)  (((set)->bits[SMB_FD_WORD(fd)] & SMB_FD_BIT(fd)) != 0)

struct timeval {
    long tv_sec;
    long tv_usec;
};

#endif /* KOSMOS_SMB_SYS_SELECT_H */
