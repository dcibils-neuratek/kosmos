/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SYS_TYPES_H
#define SYS_TYPES_H

/*
 * **The POSIX layer's `<sys/types.h>`** (`docs/w3.md`), in place of the
 * runtime's for ported code only: `runtime/posix/include/` comes first on
 * their include path, and on nobody else's. The runtime's holds `mode_t`
 * and nothing more, on purpose; a port asks for the rest. `clock_t` stays
 * `<time.h>`'s.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

typedef unsigned int mode_t;
typedef int64_t      off_t;
typedef long         ssize_t;
typedef int          pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int useconds_t;
typedef long         suseconds_t;
typedef uint64_t     ino_t;
typedef uint64_t     dev_t;
typedef uint32_t     nlink_t;
typedef long         blksize_t;
typedef int64_t      blkcnt_t;

__END_DECLS

#endif /* SYS_TYPES_H */
