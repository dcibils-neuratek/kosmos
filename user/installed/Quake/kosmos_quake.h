/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Included ahead of every Quake source file, by `QUAKE_CFLAGS`.
 *
 * For the POSIX names the engine calls that the SDL shim has no business
 * answering. The first: `console.c` deletes `qconsole.log` with `unlink` when
 * started with `-condebug`. Kosmos's `unistd.h` is empty on purpose -
 * `CLAUDE.md` puts `unistd.h` on the list of what a POSIX personality is made
 * of - so the call goes to ISO C's `remove` instead, which does the same job,
 * is declared in `stdio.h`, and which this libc answers by refusing: there is
 * no tree to delete from. No POSIX name is added to Kosmos to get there.
 */
#ifndef KOSMOS_QUAKE_H
#define KOSMOS_QUAKE_H

#define unlink(path) remove(path)

/*
 * And the log `-condebug` appends to, which `Con_DebugLog` writes with POSIX's
 * `open`, `write` and `close` on a file descriptor, and does not check. There
 * are no file descriptors here - `CLAUDE.md` names global file descriptors
 * as part of a POSIX personality - so opening fails, and writing and closing
 * do nothing. The console still prints; the log file is not written. The
 * arguments that are not used vanish with the call, which is also what takes
 * the undefined `O_WRONLY | O_CREAT | O_APPEND` with them.
 *
 * Safe as function-like macros because those three lines are the only calls
 * to these names anywhere in the engine this build compiles, and no header in
 * it declares anything called `open`, `write` or `close`.
 */
#define open(path, flags, mode) (-1)
#define write(fd, buf, count)   (-1)
#define close(fd)               (-1)

#endif
