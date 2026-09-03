#ifndef SYS_STAT_H
#define SYS_STAT_H

/*
 * Enough of `<sys/stat.h>` for a port that wants to make a directory.
 *
 * There is no `stat` here and there is not going to be one: a `struct stat`
 * describes a file found by a global path, and this system has no global
 * tree to find one in. What a Kosmos process has is a namespace it was
 * handed, and the way to ask about something in it is the filesystem
 * protocol, from Lua.
 *
 * `mkdir` is declared and fails. See `stdio.c` for the same argument at
 * greater length: failing is useful, because the caller checks and reports;
 * pretending to succeed loses whatever was going to be written into the
 * directory that is not there.
 */

#include <sys/types.h>

int mkdir(const char *path, mode_t mode);

#endif /* SYS_STAT_H */
