/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_DIRENT_H
#define POSIX_DIRENT_H

/*
 * **A folder's names, for ported code** (`docs/w3.md` W3d2): the Storage
 * Kit's listing, a page at a time. No `.` and no `..`, which no server
 * here lists - a port that skips them finds nothing to skip - and
 * `d_type` is `DT_UNKNOWN`, which POSIX allows: a caller that needs to
 * know asks `stat`.
 */

#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

struct dirent {
    ino_t         d_ino;
    off_t         d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char          d_name[256];
};

typedef struct posix_dir DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *d);
void           rewinddir(DIR *d);
int            closedir(DIR *d);

__END_DECLS

#endif /* POSIX_DIRENT_H */
