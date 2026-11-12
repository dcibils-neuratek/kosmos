/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef UNISTD_H
#define UNISTD_H

/*
 * **The POSIX layer's `<unistd.h>`** (`docs/w3.md`), in place of the
 * runtime's empty one for ported code only. What is declared here is
 * built in `runtime/posix/`; what a port calls and is not here fails at the
 * link, with its name, as the runtime's has it - `fork`, `exec` and the
 * rest of a personality are never here (`design.md` §17.3).
 *
 * W3b brought `sysconf(_SC_PAGESIZE)`, W3c the process's own facts and
 * sleeping; W3d brings the descriptors.
 */

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

int isatty(int fd);             /* 0 for every one (`stdio.c`) */

/* glibc's numbers, so a port's own table of them agrees. */
#define _SC_PAGESIZE          30
#define _SC_PAGE_SIZE         _SC_PAGESIZE
#define _SC_NPROCESSORS_CONF  83
#define _SC_NPROCESSORS_ONLN  84
#define _SC_PHYS_PAGES        85     /* the machine's pages, all of them */
#define _SC_AVPHYS_PAGES      86     /* and those free now */

long sysconf(int name);

/* The process ends at once, nothing run on the way (`process.c`). */
__attribute__((noreturn)) void _exit(int status);
int  getpagesize(void);

/* The process table's id for this process (`SYS_SELF`): a number to print
 * and name a file by, which reaches nothing. */
pid_t getpid(void);

/* There are no users, so there is nobody else to be: 0, always. */
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);

/* Files (W3d2), on descriptors private to this process. 0, 1 and 2 are the
 * console: a write to 1 or 2 is printed, a read of 0 is the end. */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

ssize_t read(int fd, void *buf, size_t bytes);
ssize_t write(int fd, const void *buf, size_t bytes);
ssize_t pread(int fd, void *buf, size_t bytes, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t bytes, off_t offset);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);
int     ftruncate(int fd, off_t size);
int     truncate(const char *path, off_t size);
int     fsync(int fd);
int     fdatasync(int fd);
int     dup(int fd);
int     dup2(int fd, int to);
int     unlink(const char *path);
int     rmdir(const char *path);
int     rename(const char *from, const char *to);
int     access(const char *path, int mode);
char   *getcwd(char *buf, size_t size);
int     chdir(const char *path);     /* only to where it is: ENOTSUP (`fd.c`) */
int     unlinkat(int dirfd, const char *path, int flags);

/* There are no links (`fd.c`): readlink EINVAL, link EPERM, symlink
 * ENOTSUP. */
ssize_t readlink(const char *path, char *buf, size_t room);
int     link(const char *from, const char *to);
int     symlink(const char *target, const char *path);

#define _PC_NAME_MAX 3
#define _PC_PATH_MAX 4
long    pathconf(const char *path, int name);

unsigned int sleep(unsigned int seconds);
int          usleep(useconds_t microseconds);

__END_DECLS

#endif /* UNISTD_H */
