/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The disk server's doors: what one may ask (`diskdoor.c`).
 */

#ifndef KOSMOS_DISKDOOR_H
#define KOSMOS_DISKDOOR_H

#include <stdbool.h>
#include <stddef.h>

#include "diskproto.h"

/* Whether `path` (`len` bytes at most) is in `root`, "Home/Cache/Maps". */
bool disk_door_inside(const char *root, const char *path, size_t len);

/* Whether `path` is one of the folders above `root`. */
bool disk_door_above(const char *root, const char *path);

/* Whether a door into `root` may ask this; `restricted` for every door but
 * the first. */
bool disk_door_allows(const struct disk_request *rq, const char *root, bool restricted);

#endif
