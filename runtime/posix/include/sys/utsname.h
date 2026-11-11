/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SYS_UTSNAME_H
#define SYS_UTSNAME_H

/* `uname`, for ported code (`docs/w3.md` W3c): the system's name, the
 * version and the build it was made from, and the processor. */

#include <sys/cdefs.h>

__BEGIN_DECLS

#define UTSNAME_LENGTH 65

struct utsname {
    char sysname[UTSNAME_LENGTH];   /* "Kosmos" */
    char nodename[UTSNAME_LENGTH];  /* "kosmos": there are no host names */
    char release[UTSNAME_LENGTH];   /* the version, 0.11.375 */
    char version[UTSNAME_LENGTH];   /* the build it was made from */
    char machine[UTSNAME_LENGTH];   /* "x86_64" or "aarch64" */
};

int uname(struct utsname *out);

__END_DECLS

#endif /* SYS_UTSNAME_H */
