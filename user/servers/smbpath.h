/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A name inside an SMB share, as a path on the disk (`smbpath.c`).
 */

#ifndef KOSMOS_SMBPATH_H
#define KOSMOS_SMBPATH_H

#include <stdbool.h>
#include <stddef.h>

/* `root` and `name` (backslashes or slashes) as one path in `out`, or
 * false: a "." or ".." part, a ":", or longer than `room`. */
bool smb_share_path(const char *root, const char *name, char *out, size_t room);

#endif
