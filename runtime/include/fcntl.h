/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef FCNTL_H
#define FCNTL_H

/*
 * Deliberately empty, and that is the whole content of the decision.
 *
 * `CLAUDE.md` forbids a POSIX personality by name - `fork`, `exec`,
 * `signal`, `pipe`, `socket`, `select`, `ioctl`, `unistd.h`, global file
 * descriptors - and `open` belongs on that list with the rest of them.
 * There is no such call here and there is not going to be one: what a
 * process reaches is what it was handed, through its own capability table,
 * and a path opened by name is the ambient authority the whole design
 * exists to refuse.
 *
 * This file exists because a *vendored* source includes it and never calls
 * anything from it. `runtime/upstream/doom/i_input.c` opens with eight
 * includes it inherited from a program that ran on Unix; Doom's input here
 * arrives through `doomgeneric`, and the include is a line nobody removed.
 * Removing it ourselves is the one thing the rule about vendored code
 * forbids, so the include is answered instead - with nothing.
 *
 * **A header that declares nothing cannot be a personality.** What the ARM
 * build has is worse and had gone unnoticed: newlib's `fcntl.h` comes with
 * the toolchain and declares `open`, `creat` and the rest, so those names
 * have been visible to every vendored file this system compiles. Nothing
 * calls them and nothing could link if it did - but the declarations were
 * there, and nobody chose them. This is the third header found the same
 * way, after `<inttypes.h>` and newlib's `libm`, and the pattern is the
 * point: **a build that compiles is not evidence that a dependency was
 * decided on.**
 */

#endif /* FCNTL_H */
