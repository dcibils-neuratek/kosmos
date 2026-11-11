/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Descriptors** (`docs/w3.md` W3d2): `open`, `read`, `write` and the rest
 * on the Storage Kit, for ported code.
 *
 * **A descriptor is a path and a position.** No server hands out a handle,
 * so an open file is its whole path, made absolute when it was opened, its
 * offset and its flags. `dup` shares one - POSIX's shared offset - and the
 * table, private to this process, grows and never shrinks. A file renamed
 * or deleted while open is lost to its descriptor, where a server with
 * handles would keep it; that is the price of servers that keep nothing
 * per client, and it is written here rather than discovered.
 *
 * **0, 1 and 2 are the console**: a write to 1 or 2 is printed, as `printf`
 * is; a read of 0 is the end of the file, since a ported library does not
 * read a person's keys. They cannot be closed or replaced.
 *
 * **One mutex** guards the table and every file's offset. It is never held
 * across a request to a server: a read takes the path and the offset under
 * it, asks with neither held, and moves the offset after. Two threads
 * reading one descriptor at once may therefore both read from the same
 * place, which POSIX leaves to the caller.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kosmos.h"
#include "storage.h"

#define CONSOLE 3                       /* 0, 1 and 2 */

struct open_file {
    int      refs;
    int      flags;                     /* O_ACCMODE, O_APPEND, O_NONBLOCK */
    int      dir;
    uint64_t offset;
    char     path[STORAGE_PATH_MAX];
};

struct slot {
    struct open_file *file;
    int               cloexec;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct slot *slots;
static int room;

/*
 *--------------------------------------------------------------------------
 * The kit's answers, as errno.
 */

static int fails(int e)
{
    errno = e;
    return -1;
}

static int storage_errno(long r)
{
    switch (r) {
    case STORAGE_ERR_NO_MOUNT:  return ENOENT;
    case STORAGE_ERR_NO_FILE:   return ENOENT;
    case STORAGE_ERR_NOT_DIR:   return ENOTDIR;
    case STORAGE_ERR_IS_DIR:    return EISDIR;
    case STORAGE_ERR_EXISTS:    return EEXIST;
    case STORAGE_ERR_NOT_EMPTY: return ENOTEMPTY;
    case STORAGE_ERR_FULL:      return ENOSPC;
    case STORAGE_ERR_READ_ONLY: return EROFS;
    case STORAGE_ERR_DENIED:    return EACCES;
    case STORAGE_ERR_CROSSES:   return EXDEV;
    case STORAGE_ERR_TOO_LONG:  return ENAMETOOLONG;
    case STORAGE_ERR_BAD:       return EINVAL;
    default:                    return EIO;
    }
}

static int kit_fails(long r)
{
    return fails(storage_errno(r));
}

/*
 *--------------------------------------------------------------------------
 * The table.
 */

/* The lowest free descriptor at or above `from`, the table grown to hold
 * it; -1 when memory is gone. With the lock held. */
static int free_slot(int from)
{
    int fd;

    if (from < CONSOLE) {
        from = CONSOLE;
    }

    for (fd = from; fd < room; fd++) {
        if (slots[fd].file == NULL) {
            return fd;
        }
    }

    {
        int grown = room == 0 ? 64 : room * 2;
        struct slot *more;

        while (grown <= from) {
            grown *= 2;
        }

        more = realloc(slots, (size_t)grown * sizeof(*more));

        if (more == NULL) {
            return -1;
        }

        memset(more + room, 0, (size_t)(grown - room) * sizeof(*more));
        fd = room > from ? room : from;
        slots = more;
        room = grown;
        return fd;
    }
}

/* The open file behind `fd`, with the lock held; NULL for one not open. */
static struct open_file *file_of(int fd)
{
    return fd >= CONSOLE && fd < room ? slots[fd].file : NULL;
}

/* What a call needs of a descriptor, copied out under the lock. */
struct taken {
    int      flags, dir;
    uint64_t offset;
    char     path[STORAGE_PATH_MAX];
};

static int take(int fd, struct taken *t)
{
    struct open_file *f;

    pthread_mutex_lock(&lock);
    f = file_of(fd);

    if (f != NULL) {
        t->flags = f->flags;
        t->dir = f->dir;
        t->offset = f->offset;
        memcpy(t->path, f->path, sizeof(t->path));
    }

    pthread_mutex_unlock(&lock);
    return f != NULL ? 0 : fails(EBADF);
}

/* `fd`'s offset set to `to`, if it is still the file it was. */
static void moved(int fd, uint64_t to)
{
    struct open_file *f;

    pthread_mutex_lock(&lock);

    if ((f = file_of(fd)) != NULL) {
        f->offset = to;
    }

    pthread_mutex_unlock(&lock);
}

static int readable(int flags)
{
    return (flags & O_ACCMODE) != O_WRONLY;
}

static int writable(int flags)
{
    return (flags & O_ACCMODE) != O_RDONLY;
}

/*
 *--------------------------------------------------------------------------
 * Opening and closing.
 */

int open(const char *path, int flags, ...)
{
    struct open_file *f;
    struct storage_stat st;
    char whole[STORAGE_PATH_MAX];
    int r, fd, there;

    if (path == NULL) {
        return fails(EFAULT);
    }

    if ((r = storage_absolute(path, whole)) != STORAGE_OK) {
        return kit_fails(r);
    }

    r = storage_stat(whole, &st);

    if (r != STORAGE_OK && r != STORAGE_ERR_NO_FILE) {
        return kit_fails(r);
    }

    there = r == STORAGE_OK;

    if (there) {
        if ((flags & O_CREAT) && (flags & O_EXCL)) {
            return fails(EEXIST);
        }

        if (st.kind == STORAGE_KIND_DIR && writable(flags)) {
            return fails(EISDIR);
        }

        if (st.kind != STORAGE_KIND_DIR && (flags & O_DIRECTORY)) {
            return fails(ENOTDIR);
        }

        if (!st.writable && (writable(flags) || (flags & O_TRUNC))) {
            return fails(EROFS);
        }

        if ((flags & O_TRUNC) && writable(flags) && st.kind == STORAGE_KIND_FILE
            && (r = storage_truncate(whole, 0)) != STORAGE_OK) {
            return kit_fails(r);
        }
    } else {
        long made;

        if (!(flags & O_CREAT)) {
            return fails(ENOENT);
        }

        /* Asked, then made: two requests, so two processes making one file
         * at once can both succeed (`w3.md` W3d2). */
        if ((made = storage_write(whole, 0, NULL, 0, STORAGE_CREATE)) < 0) {
            return kit_fails(made);
        }
    }

    if ((f = malloc(sizeof(*f))) == NULL) {
        return fails(ENOMEM);
    }

    f->refs = 1;
    f->flags = flags & (O_ACCMODE | O_APPEND | O_NONBLOCK);
    f->dir = there && st.kind == STORAGE_KIND_DIR;
    f->offset = 0;
    memcpy(f->path, whole, sizeof(f->path));

    pthread_mutex_lock(&lock);
    fd = free_slot(CONSOLE);

    if (fd >= 0) {
        slots[fd].file = f;
        slots[fd].cloexec = (flags & O_CLOEXEC) != 0;
    }

    pthread_mutex_unlock(&lock);

    if (fd < 0) {
        free(f);
        return fails(EMFILE);
    }

    return fd;
}

int creat(const char *path, mode_t mode)
{
    (void)mode;
    return open(path, O_WRONLY | O_CREAT | O_TRUNC);
}

int close(int fd)
{
    struct open_file *f;

    if (fd >= 0 && fd < CONSOLE) {
        return 0;                       /* the console stays */
    }

    pthread_mutex_lock(&lock);
    f = file_of(fd);

    if (f != NULL) {
        slots[fd].file = NULL;

        if (--f->refs > 0) {
            f = NULL;                   /* another descriptor still has it */
        } else {
            f->refs = -1;
        }
    } else {
        fd = -1;
    }

    pthread_mutex_unlock(&lock);

    if (fd < 0) {
        return fails(EBADF);
    }

    free(f);
    return 0;
}

static int duplicate(int fd, int to, int exact, int cloexec)
{
    struct open_file *f;
    int got = -1, e = EBADF;

    if (fd >= 0 && fd < CONSOLE) {
        return fails(EINVAL);           /* the console is not a file to share */
    }

    if (to < 0 || (exact && to < CONSOLE)) {
        return fails(exact ? EINVAL : EBADF);
    }

    pthread_mutex_lock(&lock);
    f = file_of(fd);

    if (f != NULL) {
        if (exact && to == fd) {
            got = fd;
        } else {
            struct open_file *old = NULL;

            got = exact ? (to < room ? to : free_slot(to)) : free_slot(to);

            if (got < 0) {
                e = EMFILE;
            } else {
                old = slots[got].file;

                if (old != NULL && --old->refs > 0) {
                    old = NULL;
                }

                slots[got].file = f;
                slots[got].cloexec = cloexec;
                f->refs++;
                pthread_mutex_unlock(&lock);
                free(old);
                return got;
            }
        }
    }

    pthread_mutex_unlock(&lock);
    return got >= 0 ? got : fails(e);
}

int dup(int fd)
{
    return duplicate(fd, CONSOLE, 0, 0);
}

int dup2(int fd, int to)
{
    return duplicate(fd, to, 1, 0);
}

int fcntl(int fd, int command, ...)
{
    struct open_file *f;
    va_list ap;
    int arg, r = 0;

    va_start(ap, command);
    arg = va_arg(ap, int);
    va_end(ap);

    if (command == F_DUPFD || command == F_DUPFD_CLOEXEC) {
        return duplicate(fd, arg, 0, command == F_DUPFD_CLOEXEC);
    }

    if (fd >= 0 && fd < CONSOLE) {
        return command == F_GETFL ? (fd == 0 ? O_RDONLY : O_WRONLY) : 0;
    }

    pthread_mutex_lock(&lock);
    f = file_of(fd);

    if (f == NULL) {
        r = -1;
    } else {
        switch (command) {
        case F_GETFD: r = slots[fd].cloexec ? FD_CLOEXEC : 0; break;
        case F_SETFD: slots[fd].cloexec = (arg & FD_CLOEXEC) != 0; break;
        case F_GETFL: r = f->flags; break;
        case F_SETFL:
            /* Only these two change after opening, as on Linux. */
            f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | (arg & (O_APPEND | O_NONBLOCK));
            break;
        default: r = -2; break;
        }
    }

    pthread_mutex_unlock(&lock);
    return r == -1 ? fails(EBADF) : r == -2 ? fails(EINVAL) : r;
}

/*
 *--------------------------------------------------------------------------
 * Bytes.
 */

static ssize_t console_write(const void *buf, size_t bytes)
{
    if (bytes > 0 && kosmos_write(buf, bytes) < 0) {
        return fails(EIO);
    }

    return (ssize_t)bytes;
}

ssize_t pread(int fd, void *buf, size_t bytes, off_t offset)
{
    struct taken t;
    long r;

    if (fd >= 0 && fd < CONSOLE) {
        return fd == 0 ? 0 : fails(ESPIPE);
    }

    if (take(fd, &t) != 0) {
        return -1;
    }

    if (offset < 0) {
        return fails(EINVAL);
    }

    if (t.dir) {
        return fails(EISDIR);
    }

    if (!readable(t.flags)) {
        return fails(EBADF);
    }

    r = storage_read(t.path, (uint64_t)offset, buf, bytes);
    return r < 0 ? kit_fails(r) : (ssize_t)r;
}

ssize_t read(int fd, void *buf, size_t bytes)
{
    struct taken t;
    ssize_t n;

    if (fd == 0) {
        return 0;
    }

    if (fd > 0 && fd < CONSOLE) {
        return fails(EBADF);
    }

    if (take(fd, &t) != 0) {
        return -1;
    }

    if ((n = pread(fd, buf, bytes, (off_t)t.offset)) > 0) {
        moved(fd, t.offset + (uint64_t)n);
    }

    return n;
}

static ssize_t write_at(const struct taken *t, const void *buf, size_t bytes, uint64_t at)
{
    long r;

    if (t->dir) {
        return fails(EISDIR);
    }

    if (!writable(t->flags)) {
        return fails(EBADF);
    }

    r = storage_write(t->path, at, buf, bytes, 0);
    return r < 0 ? kit_fails(r) : (ssize_t)r;
}

ssize_t pwrite(int fd, const void *buf, size_t bytes, off_t offset)
{
    struct taken t;

    if (fd == 1 || fd == 2) {
        return console_write(buf, bytes);
    }

    if (fd == 0) {
        return fails(EBADF);
    }

    if (take(fd, &t) != 0) {
        return -1;
    }

    if (offset < 0) {
        return fails(EINVAL);
    }

    return write_at(&t, buf, bytes, (uint64_t)offset);
}

ssize_t write(int fd, const void *buf, size_t bytes)
{
    struct taken t;
    uint64_t at;
    ssize_t n;

    if (fd == 1 || fd == 2) {
        return console_write(buf, bytes);
    }

    if (fd == 0) {
        return fails(EBADF);
    }

    if (take(fd, &t) != 0) {
        return -1;
    }

    at = t.offset;

    /* At the end as it is now, asked each time: another descriptor, or
     * another process, may have made it longer since. */
    if (t.flags & O_APPEND) {
        struct storage_stat st;
        int r = storage_stat(t.path, &st);

        if (r != STORAGE_OK) {
            return kit_fails(r);
        }

        at = st.size;
    }

    if ((n = write_at(&t, buf, bytes, at)) >= 0) {
        moved(fd, at + (uint64_t)n);
    }

    return n;
}

off_t lseek(int fd, off_t offset, int whence)
{
    struct taken t;
    int64_t base;

    if (fd >= 0 && fd < CONSOLE) {
        return fails(ESPIPE);
    }

    if (take(fd, &t) != 0) {
        return -1;
    }

    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int64_t)t.offset; break;
    case SEEK_END: {
        struct storage_stat st;
        int r = storage_stat(t.path, &st);

        if (r != STORAGE_OK) {
            return kit_fails(r);
        }

        base = (int64_t)st.size;
        break;
    }
    default:
        return fails(EINVAL);
    }

    if (base + offset < 0) {
        return fails(EINVAL);
    }

    moved(fd, (uint64_t)(base + offset));
    return base + offset;
}

int ftruncate(int fd, off_t size)
{
    struct taken t;
    int r;

    if (take(fd, &t) != 0) {
        return -1;
    }

    if (size < 0 || t.dir) {
        return fails(EINVAL);
    }

    if (!writable(t.flags)) {
        return fails(EBADF);            /* EINVAL on Linux; EBADF is POSIX's */
    }

    return (r = storage_truncate(t.path, (uint64_t)size)) == STORAGE_OK ? 0 : kit_fails(r);
}

int truncate(const char *path, off_t size)
{
    int r;

    if (size < 0) {
        return fails(EINVAL);
    }

    return (r = storage_truncate(path, (uint64_t)size)) == STORAGE_OK ? 0 : kit_fails(r);
}

int fsync(int fd)
{
    struct taken t;
    int r;

    if (fd >= 0 && fd < CONSOLE) {
        return 0;
    }

    if (take(fd, &t) != 0) {
        return -1;
    }

    return (r = storage_flush(t.path)) == STORAGE_OK ? 0 : kit_fails(r);
}

int fdatasync(int fd)
{
    return fsync(fd);
}

/*
 *--------------------------------------------------------------------------
 * What a path is.
 */

/* FNV-1a of the whole path: the same file, the same number, which is all
 * a port compares inodes for. */
static ino_t inode_of(const char *path)
{
    uint64_t h = 1469598103934665603ULL;

    for (; *path != '\0'; path++) {
        h = (h ^ (unsigned char)*path) * 1099511628211ULL;
    }

    return h != 0 ? h : 1;
}

static void stat_from(const char *whole, const struct storage_stat *st, struct stat *out)
{
    memset(out, 0, sizeof(*out));
    out->st_dev = 1;
    out->st_ino = inode_of(whole);
    out->st_nlink = 1;

    if (st->kind == STORAGE_KIND_DIR) {
        out->st_mode = S_IFDIR | (st->writable ? 0755 : 0555);
    } else {
        out->st_mode = S_IFREG | (st->writable ? 0644 : 0444);
        out->st_size = (off_t)st->size;
    }

    out->st_blksize = 4096;
    out->st_blocks = (blkcnt_t)((st->size + 511) / 512);
    out->st_mtim.tv_sec = (time_t)st->modified;
    out->st_atim = out->st_mtim;
    out->st_ctim = out->st_mtim;
}

int stat(const char *path, struct stat *out)
{
    struct storage_stat st;
    char whole[STORAGE_PATH_MAX];
    int r;

    if (path == NULL || out == NULL) {
        return fails(EFAULT);
    }

    if ((r = storage_absolute(path, whole)) != STORAGE_OK
        || (r = storage_stat(whole, &st)) != STORAGE_OK) {
        return kit_fails(r);
    }

    stat_from(whole, &st, out);
    return 0;
}

int lstat(const char *path, struct stat *out)
{
    return stat(path, out);
}

int fstat(int fd, struct stat *out)
{
    struct storage_stat st;
    struct taken t;
    int r;

    if (out == NULL) {
        return fails(EFAULT);
    }

    if (fd >= 0 && fd < CONSOLE) {
        memset(out, 0, sizeof(*out));
        out->st_mode = S_IFCHR | 0620;
        out->st_nlink = 1;
        out->st_blksize = 1024;
        return 0;
    }

    if (take(fd, &t) != 0) {
        return -1;
    }

    if ((r = storage_stat(t.path, &st)) != STORAGE_OK) {
        return kit_fails(r);
    }

    stat_from(t.path, &st, out);
    return 0;
}

int access(const char *path, int mode)
{
    struct storage_stat st;
    int r;

    if ((r = storage_stat(path, &st)) != STORAGE_OK) {
        return kit_fails(r);
    }

    if ((mode & W_OK) && !st.writable) {
        return fails(EROFS);
    }

    if ((mode & X_OK) && st.kind != STORAGE_KIND_DIR) {
        return fails(EACCES);           /* nothing here is run by its bits */
    }

    return 0;
}

int chmod(const char *path, mode_t mode)
{
    struct storage_stat st;
    int r;

    (void)mode;
    return (r = storage_stat(path, &st)) == STORAGE_OK ? 0 : kit_fails(r);
}

int fchmod(int fd, mode_t mode)
{
    struct taken t;

    (void)mode;
    return take(fd, &t);
}

mode_t umask(mode_t mask)
{
    (void)mask;
    return 022;
}

/*
 *--------------------------------------------------------------------------
 * Changing the tree.
 */

int mkdir(const char *path, mode_t mode)
{
    int r;

    (void)mode;
    return (r = storage_mkdir(path)) == STORAGE_OK ? 0 : kit_fails(r);
}

int unlink(const char *path)
{
    struct storage_stat st;
    int r;

    if ((r = storage_stat(path, &st)) != STORAGE_OK) {
        return kit_fails(r);
    }

    if (st.kind == STORAGE_KIND_DIR) {
        return fails(EISDIR);
    }

    return (r = storage_delete(path)) == STORAGE_OK ? 0 : kit_fails(r);
}

int rmdir(const char *path)
{
    struct storage_stat st;
    int r;

    if ((r = storage_stat(path, &st)) != STORAGE_OK) {
        return kit_fails(r);
    }

    if (st.kind != STORAGE_KIND_DIR) {
        return fails(ENOTDIR);
    }

    return (r = storage_delete(path)) == STORAGE_OK ? 0 : kit_fails(r);
}

int remove(const char *path)
{
    int r = storage_delete(path);

    return r == STORAGE_OK ? 0 : kit_fails(r);
}

int rename(const char *from, const char *to)
{
    struct storage_stat st;
    int r;

    /* POSIX replaces what is at `to`; the servers refuse, so a file in
     * the way is deleted first - two requests, as `O_EXCL` is. */
    if (storage_stat(to, &st) == STORAGE_OK && st.kind == STORAGE_KIND_FILE) {
        struct storage_stat mine;

        if ((r = storage_stat(from, &mine)) != STORAGE_OK) {
            return kit_fails(r);
        }

        if (mine.kind == STORAGE_KIND_DIR) {
            return fails(ENOTDIR);
        }

        if ((r = storage_delete(to)) != STORAGE_OK) {
            return kit_fails(r);
        }
    }

    return (r = storage_rename(from, to)) == STORAGE_OK ? 0 : kit_fails(r);
}

/*
 *--------------------------------------------------------------------------
 * Where.
 */

char *getcwd(char *buf, size_t size)
{
    const char *here = storage_cwd();
    size_t n = strlen(here) + 1;

    if (buf == NULL) {
        if (size == 0) {
            size = n;
        }

        if (size < n) {
            errno = ERANGE;
            return NULL;
        }

        if ((buf = malloc(size)) == NULL) {
            errno = ENOMEM;
            return NULL;
        }
    } else if (size < n) {
        errno = ERANGE;
        return NULL;
    }

    memcpy(buf, here, n);
    return buf;
}

char *realpath(const char *path, char *resolved)
{
    struct storage_stat st;
    char whole[STORAGE_PATH_MAX];
    int r;

    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if ((r = storage_absolute(path, whole)) != STORAGE_OK
        || (r = storage_stat(whole, &st)) != STORAGE_OK) {
        errno = storage_errno(r);
        return NULL;
    }

    if (resolved == NULL && (resolved = malloc(PATH_MAX)) == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    snprintf(resolved, PATH_MAX, "%s", whole);
    return resolved;
}

/*
 *--------------------------------------------------------------------------
 * Folders.
 */

struct posix_dir {
    char          path[STORAGE_PATH_MAX];
    uint64_t      cursor;
    long          left;                 /* names in `names` not yet given */
    const char   *next;
    int           done;
    struct dirent entry;
    char          names[2048];
};

DIR *opendir(const char *path)
{
    struct storage_stat st;
    DIR *d;
    int r;

    if ((d = calloc(1, sizeof(*d))) == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    if ((r = storage_absolute(path, d->path)) != STORAGE_OK
        || (r = storage_stat(d->path, &st)) != STORAGE_OK) {
        free(d);
        errno = storage_errno(r);
        return NULL;
    }

    if (st.kind != STORAGE_KIND_DIR) {
        free(d);
        errno = ENOTDIR;
        return NULL;
    }

    return d;
}

struct dirent *readdir(DIR *d)
{
    size_t len;

    if (d == NULL) {
        errno = EBADF;
        return NULL;
    }

    if (d->left == 0 && !d->done) {
        long n = storage_list(d->path, &d->cursor, d->names, sizeof(d->names));

        if (n < 0) {
            errno = storage_errno(n);
            return NULL;
        }

        d->left = n;
        d->next = d->names;
        d->done = n == 0;
    }

    if (d->left == 0) {
        return NULL;                    /* the end, errno untouched */
    }

    len = strlen(d->next);

    if (len >= sizeof(d->entry.d_name)) {
        len = sizeof(d->entry.d_name) - 1;
    }

    memset(&d->entry, 0, sizeof(d->entry));
    memcpy(d->entry.d_name, d->next, len);
    d->entry.d_type = DT_UNKNOWN;
    d->entry.d_reclen = (unsigned short)sizeof(d->entry);
    d->entry.d_off = (off_t)d->cursor;

    {
        char whole[STORAGE_PATH_MAX];

        snprintf(whole, sizeof whole, "%s/%s", strcmp(d->path, "/") == 0 ? "" : d->path,
                 d->entry.d_name);
        d->entry.d_ino = inode_of(whole);
    }

    d->next += strlen(d->next) + 1;
    d->left--;
    return &d->entry;
}

void rewinddir(DIR *d)
{
    if (d != NULL) {
        d->cursor = 0;
        d->left = 0;
        d->done = 0;
    }
}

int closedir(DIR *d)
{
    if (d == NULL) {
        return fails(EBADF);
    }

    free(d);
    return 0;
}
