/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_SYS_UIO_H
#define POSIX_SYS_UIO_H

/*
 * **`struct iovec`** (`docs/w5.md` W5c): a pointer and a length, which
 * SQLite's `carray` takes arrays of and `sys/socket.h`'s `msghdr` points
 * at. The structure only - nothing here reads or writes a vector yet.
 */

#include <sys/cdefs.h>
#include <sys/types.h>

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

#endif /* POSIX_SYS_UIO_H */
