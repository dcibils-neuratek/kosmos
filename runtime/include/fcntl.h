/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef FCNTL_H
#define FCNTL_H

#include <sys/cdefs.h>
__BEGIN_DECLS

/*
 * Deliberately empty here, in the runtime that Kosmos's own code and every
 * vendored file are built against.
 *
 * Kosmos's own kits, servers and applications do not open files by name
 * from C: what they reach is what they were handed, through the namespace
 * and the file protocols. This file exists because a *vendored* source
 * includes it and never calls anything from it -
 * `runtime/upstream/doom/i_input.c` opens with eight includes inherited
 * from a program that ran on Unix, and removing one is the thing the rule
 * about vendored code forbids, so the include is answered with nothing.
 *
 * **Ported code has an `open`, elsewhere.** This said there was none "and
 * there is not going to be one", until Diego amended the principle on 11
 * November 2026 (`CLAUDE.md`, *Compatibility inside a process*): the POSIX
 * names inside a process, for ported code, in `runtime/posix/`, whose
 * `fcntl.h` shadows this one for what links it (`docs/w3.md` W3d2). What
 * stays true is the reason this one is empty: a path there resolves in the
 * process's own namespace, through the Storage Kit, and a descriptor never
 * leaves the process - the ambient authority this file refused is still
 * refused, by a different file.
 *
 * And a finding worth keeping: newlib's `fcntl.h` came with the ARM
 * toolchain and declared `open` to every vendored file until this one
 * shadowed it. **A build that compiles is not evidence that a dependency
 * was decided on.**
 */

__END_DECLS

#endif /* FCNTL_H */
