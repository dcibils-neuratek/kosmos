/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef UNISTD_H
#define UNISTD_H

#include <sys/cdefs.h>
__BEGIN_DECLS

/*
 * Empty, and empty on purpose.
 *
 * `CLAUDE.md` names `unistd.h` in the list of things a POSIX personality is
 * made of, alongside `fork`, `exec`, `signal`, `pipe` and global file
 * descriptors, and forbids all of it at system level. This file does not
 * bring any of that back.
 *
 * What it does is stop the *toolchain's* one being reached. newlib ships a
 * `<unistd.h>` and it is the real thing - `fork`, `execve`, `getuid`, a
 * hundred declarations that would compile and then fail to link, or worse
 * would find something with a matching name. `-Iruntime/include` comes
 * first, so a vendored port that includes it gets this instead.
 *
 * Nothing else is declared here, deliberately. A port that includes the header
 * out of habit compiles; one that actually calls `unlink` or `fork` fails
 * at the *link*, with the name of the thing it wanted. That is the boundary
 * enforced by the linker rather than by hope, which is where this project
 * likes its boundaries - the same way `string.c` calling `malloc` broke the
 * kernel build and told us so.
 *
 * The rule this follows is `CLAUDE.md`'s exactly: compatibility inside a
 * process, yes; a personality at system level, never. A header is not a
 * personality. A `fork` would be.
 */

#include <stddef.h>

/*
 * The one exception, and it says no. C++'s library asks whether a stream
 * is a terminal, through `fileno`, and nothing in Kosmos is reached by a
 * number, so `isatty` answers 0 for every one (`stdio.c`).
 */
int isatty(int fd);

__END_DECLS

#endif /* UNISTD_H */
