/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_Q3_H
#define KOSMOS_Q3_H

/*
 * **What Quake3e is told about this machine**, forced in front of every one
 * of its files with `-include` (`docs/quake3e.md` §2), so that nothing in
 * the fetched release is edited - the rule `lua/upstream/` and Quake 1's
 * `kosmos_quake.h` follow.
 *
 * - **The operating system**: `q_platform.h` knows Linux, the BSDs, macOS
 *   and Windows, and stops with "Operating system not supported" for
 *   anything else. Its Linux block is two lines and an `<endian.h>`; these
 *   are the same two lines, said for Kosmos. The architecture it finds by
 *   itself, from `__x86_64__`.
 * - **`popen` and `pclose`**: `files.c` pipes a recorded video into an
 *   external ffmpeg (`\video-pipe`). There is no external program to pipe
 *   to and no shell to start it, so `popen` answers NULL and the command
 *   says it could not open the file. Declared here, defined in
 *   `q3_kosmos.c`, and in no header of the POSIX layer, which is not where
 *   a refusal one port needs belongs.
 * - **`ctime`**: the bot library's preprocessor has `__DATE__` and
 *   `__TIME__` for its scripts, and the C library has `localtime` and
 *   `strftime` but not this older spelling of them.
 */

#define OS_STRING "kosmos"
#define ID_INLINE inline
#define Q3_LITTLE_ENDIAN

#include <stdio.h>
#include <time.h>

FILE *popen(const char *command, const char *mode);
int   pclose(FILE *stream);
char *ctime(const time_t *t);

#endif /* KOSMOS_Q3_H */
