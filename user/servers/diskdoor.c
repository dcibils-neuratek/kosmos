/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The disk server's doors (`diskfs.c`, `docs/maps.md` M6d): whether what
 * comes through one may be asked - a path in the door's folder and never
 * out of it through "." or "..", the folders above it to make, and the
 * disk itself the first door's alone. Apart from the server so the Mac can
 * hold it to its cases (`tools/test_diskdoor.c`).
 */

#include <string.h>

#include "diskdoor.h"
#include "kfs.h"

/*
 * Whether `path` is in `root` - the root's folders, one by one, then
 * anything or nothing - and never through "." or "..", which a door's
 * caller could otherwise use to climb out of it.
 */
bool disk_door_inside(const char *root, const char *path, size_t len)
{
    size_t at = 0;
    const char *r = root;

    while (*r != '\0') {
        size_t n, rn;
        const char *slash;

        while (at < len && path[at] == '/') {
            at++;
        }

        for (n = 0; at + n < len && path[at + n] != '/' && path[at + n] != '\0'; n++) {
        }

        slash = strchr(r, '/');
        rn = slash ? (size_t)(slash - r) : strlen(r);

        if (n == 0 || !kfs_same_name(path + at, n, r, rn)) {
            return false;
        }

        at += n;
        r += rn;

        if (*r == '/') {
            r++;
        }
    }

    /* Below the root, no step may go back up. */
    while (at < len && path[at] != '\0') {
        size_t n;

        while (at < len && path[at] == '/') {
            at++;
        }

        for (n = 0; at + n < len && path[at + n] != '/' && path[at + n] != '\0'; n++) {
        }

        if ((n == 1 && path[at] == '.') || (n == 2 && path[at] == '.' && path[at + 1] == '.')) {
            return false;
        }

        at += n;
    }

    return true;
}

/* Whether `path` is one of the folders above `root` - "/Home" or
 * "/Home/Cache" for "Home/Cache/Maps" - which a door may make, so its own
 * folder can be made on a disk that has none of them yet. */
bool disk_door_above(const char *root, const char *path)
{
    size_t n;

    while (*path == '/') {
        path++;
    }

    n = strlen(path);

    while (n > 0 && path[n - 1] == '/') {
        n--;
    }

    return n > 0 && n < strlen(root) && strncmp(root, path, n) == 0 && root[n] == '/';
}

/* Whether this door may ask this: the disk itself is the first door's, and
 * every path is to be in the door's folder - a rename's destination too,
 * when it is a path rather than a name. */
bool disk_door_allows(const struct disk_request *rq, const char *root, bool restricted)
{
    size_t to_len;

    switch (rq->op) {
    case DISK_OP_SUPER:
    case DISK_OP_DEVICE:
    case DISK_OP_FORMAT:
        return !restricted;
    case DISK_OP_RENAME:
        to_len = rq->length < DISK_PATH_MAX ? rq->length : 0;

        if (memchr(rq->u.to, '/', to_len) != NULL && !disk_door_inside(root, rq->u.to, to_len)) {
            return false;
        }
        break;
    case DISK_OP_MKDIR:
        if (restricted && disk_door_above(root, rq->path)) {
            return true;
        }
        break;
    default:
        break;
    }

    return disk_door_inside(root, rq->path, DISK_PATH_MAX);
}
