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

long sysconf(int name);
int  getpagesize(void);

/* The process table's id for this process (`SYS_SELF`): a number to print
 * and name a file by, which reaches nothing. */
pid_t getpid(void);

/* There are no users, so there is nobody else to be: 0, always. */
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);

unsigned int sleep(unsigned int seconds);
int          usleep(useconds_t microseconds);

__END_DECLS

#endif /* UNISTD_H */
