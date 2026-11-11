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
 * W3b brings `sysconf(_SC_PAGESIZE)`; W3c and W3d the rest.
 */

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

int isatty(int fd);             /* 0 for every one (`stdio.c`) */

#define _SC_PAGESIZE  30
#define _SC_PAGE_SIZE _SC_PAGESIZE

long sysconf(int name);
int  getpagesize(void);

__END_DECLS

#endif /* UNISTD_H */
