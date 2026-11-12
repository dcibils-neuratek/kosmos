/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_SYS_STATVFS_H
#define POSIX_SYS_STATVFS_H

/*
 * **How much room a file system has**, for ported code (`docs/w4.md`
 * W4c) - named so libc++'s `std::filesystem::space` and WebKit's
 * free-space figure compile. The Storage Kit has no request for it yet,
 * so both calls find what they were given and say ENOTSUP (`fd.c`).
 */

#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;

struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    fsblkcnt_t    f_blocks;
    fsblkcnt_t    f_bfree;
    fsblkcnt_t    f_bavail;
    fsfilcnt_t    f_files;
    fsfilcnt_t    f_ffree;
    fsfilcnt_t    f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

#define ST_RDONLY 1
#define ST_NOSUID 2

int statvfs(const char *path, struct statvfs *out);
int fstatvfs(int fd, struct statvfs *out);

__END_DECLS

#endif /* POSIX_SYS_STATVFS_H */
