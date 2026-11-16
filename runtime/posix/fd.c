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
 *
 * **A socket is a descriptor too** (W3e): an open file whose `sock` is set
 * and whose path is empty. `read`, `write`, `close`, `dup` and `fcntl` work
 * on it as on a file, `socket.c` does the rest, and the calls that need a
 * position - `pread`, `lseek` - say `ESPIPE`, as they do on Linux.
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
#include <sys/file.h>
#include <sys/sendfile.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#include "conproto.h"
#include "kosmos.h"
#include "posix.h"
#include "storage.h"

/*
 * **What a program changed is on the disk when it ends** (`docs/diskfs.md`,
 * *Grouped commits*). A disk server groups what it is asked into one commit
 * a second, and keeps it sooner only when asked - `fsync`, as anywhere. A
 * ported program that writes its files and ends, as `tcc` and a test do,
 * has never asked; with nothing done here, a machine switched off within
 * the second of its end has none of them. So each change is noted against
 * the drive it is on - its path's first name, `/Home` or `/Sandboxes` - and
 * the program's end asks each of those for its writes once
 * (`posix_changes_kept`). Never on `close`: WebKit's caches open, write and
 * close a file at a time, and a commit for each is what grouping removed.
 */
#define CHANGED_DRIVES 4

static char changed_on[CHANGED_DRIVES][64];
static pthread_mutex_t changed_lock = PTHREAD_MUTEX_INITIALIZER;

static void changed(const char *path, long r)
{
    size_t n;
    const char *end;

    if (r < 0 || path == NULL || path[0] != '/') {
        return;
    }

    end = strchr(path + 1, '/');
    n = end != NULL ? (size_t)(end - path) : strlen(path);

    if (n >= sizeof changed_on[0]) {
        return;
    }

    pthread_mutex_lock(&changed_lock);

    for (unsigned i = 0; i < CHANGED_DRIVES; i++) {
        if (changed_on[i][0] == 0) {
            memcpy(changed_on[i], path, n);
            changed_on[i][n] = 0;
            break;
        }

        if (strncmp(changed_on[i], path, n) == 0 && changed_on[i][n] == 0) {
            break;
        }
    }

    pthread_mutex_unlock(&changed_lock);
}

static int io_on;                       /* the tally below (`io_note`) */
static pthread_mutex_t io_lock;
static void io_report(void);

void posix_changes_kept(void)
{
    char drives[CHANGED_DRIVES][64];

    if (io_on > 0) {
        pthread_mutex_lock(&io_lock);
        io_report();
        pthread_mutex_unlock(&io_lock);
    }

    pthread_mutex_lock(&changed_lock);
    memcpy(drives, changed_on, sizeof drives);
    memset(changed_on, 0, sizeof changed_on);
    pthread_mutex_unlock(&changed_lock);

    for (unsigned i = 0; i < CHANGED_DRIVES && drives[i][0] != 0; i++) {
        (void)storage_flush(drives[i]);
    }
}

/*
 * **What a program writes, by file** (`testing.md` 18.836, `roadmap.md`:
 * JetSki wrote 229 MB to load one page into a `/Home` that grew 362 KB).
 * With `/Home/Settings/POSIX/io` there, every change is tallied against
 * its path - writes, bytes, truncations, removals, renames, `fsync`s -
 * and every IO_EVERY writes the busiest files by bytes go to the log,
 * with the whole since the program began. Without the file, one look at
 * the first change and nothing after.
 */
#define IO_FILES 96u
#define IO_EVERY 4000u
#define IO_SHOWN 12u

enum io_kind { IO_WRITE, IO_CUT, IO_DELETE, IO_RENAME, IO_MKDIR, IO_SYNC };

struct io_file {
    char     path[96];
    uint64_t bytes;
    uint32_t count[IO_SYNC + 1];
};

static struct io_file io_files[IO_FILES];
static uint32_t io_used, io_writes, io_dropped;
static uint64_t io_bytes;
static int io_on = -1;                  /* -1: not looked yet */
static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;

static void io_report(void)
{
    uint32_t order[IO_FILES];

    for (uint32_t i = 0; i < io_used; i++) {
        order[i] = i;
    }

    for (uint32_t i = 1; i < io_used; i++) {     /* by bytes, most first */
        uint32_t o = order[i], j = i;

        while (j > 0 && io_files[order[j - 1]].bytes < io_files[o].bytes) {
            order[j] = order[j - 1];
            j--;
        }

        order[j] = o;
    }

    fprintf(stderr, "posix: io %u writes, %llu KB, over %u files%s\n", io_writes,
            (unsigned long long)(io_bytes / 1024), io_used,
            io_dropped ? " (more not tallied)" : "");

    for (uint32_t i = 0; i < io_used && i < IO_SHOWN; i++) {
        const struct io_file *f = &io_files[order[i]];

        fprintf(stderr, "posix: io %8llu KB %6u w %4u cut %4u rm %4u mv %4u sync  %s\n",
                (unsigned long long)(f->bytes / 1024), f->count[IO_WRITE],
                f->count[IO_CUT], f->count[IO_DELETE], f->count[IO_RENAME],
                f->count[IO_SYNC], f->path);
    }
}

static void io_note(const char *path, enum io_kind kind, uint64_t bytes, long r)
{
    struct io_file *f = NULL;

    if (path == NULL || r < 0) {
        return;
    }

    if (io_on < 0) {
        struct storage_stat st;

        io_on = (storage_stat)("/Home/Settings/POSIX/io", &st) == STORAGE_OK;
    }

    if (!io_on) {
        return;
    }

    pthread_mutex_lock(&io_lock);

    for (uint32_t i = 0; i < io_used; i++) {
        if (strncmp(io_files[i].path, path, sizeof io_files[i].path - 1) == 0) {
            f = &io_files[i];
            break;
        }
    }

    if (f == NULL && io_used < IO_FILES) {
        f = &io_files[io_used++];
        snprintf(f->path, sizeof f->path, "%s", path);
    }

    if (f == NULL) {
        io_dropped++;
    } else {
        f->count[kind]++;
        f->bytes += bytes;
    }

    io_bytes += bytes;

    if (kind == IO_WRITE && ++io_writes % IO_EVERY == 0) {
        io_report();
    }

    pthread_mutex_unlock(&io_lock);
}

#define NOTED(path, call) ({ __typeof__(call) noted_r_ = (call); \
                             changed(path, (long)noted_r_); noted_r_; })
#define TALLIED(path, kind, n, call) ({ __typeof__(call) tallied_r_ = (call); \
                             io_note(path, kind, n, (long)tallied_r_); tallied_r_; })
#define storage_write(p, at, buf, n, f) \
    TALLIED(p, IO_WRITE, n, NOTED(p, (storage_write)(p, at, buf, n, f)))
#define storage_truncate(p, ...) TALLIED(p, IO_CUT, 0, NOTED(p, (storage_truncate)(p, __VA_ARGS__)))
#define storage_delete(p, ...)   TALLIED(p, IO_DELETE, 0, NOTED(p, (storage_delete)(p, ##__VA_ARGS__)))
#define storage_rename(p, ...)   TALLIED(p, IO_RENAME, 0, NOTED(p, (storage_rename)(p, __VA_ARGS__)))
#define storage_mkdir(p)         TALLIED(p, IO_MKDIR, 0, NOTED(p, (storage_mkdir)(p)))

#define CONSOLE 3                       /* 0, 1 and 2 */

struct open_file {
    int      refs;
    int      flags;                     /* O_ACCMODE, O_APPEND, O_NONBLOCK */
    int      dir;
    struct posix_socket *sock;          /* a socket's, NULL for a file */
    struct posix_pipe *pipe;            /* a pipe's end, NULL otherwise (`pipe.c`) */
    int      pipe_end;                  /* 0 reads, 1 writes */
    uint64_t offset;
    int      unlinked;                  /* removed at the last close (`unlink`) */
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
    int is_socket = 0;

    pthread_mutex_lock(&lock);
    f = file_of(fd);

    if (f != NULL && (f->sock != NULL || f->pipe != NULL)) {
        is_socket = 1;
    } else if (f != NULL) {
        t->flags = f->flags;
        t->dir = f->dir;
        t->offset = f->offset;
        memcpy(t->path, f->path, sizeof(t->path));
    }

    pthread_mutex_unlock(&lock);

    if (f == NULL) {
        return fails(EBADF);
    }

    return is_socket ? fails(ESPIPE) : 0;
}

/*
 *--------------------------------------------------------------------------
 * Sockets' doors into the table (`posix.h`).
 */

int posix_fd_socket_new(struct posix_socket *s, int flags)
{
    struct open_file *f = calloc(1, sizeof(*f));
    int fd;

    if (f == NULL) {
        return fails(ENOMEM);
    }

    f->refs = 1;
    f->flags = O_RDWR | (flags & O_NONBLOCK);
    f->sock = s;

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

struct posix_socket *posix_fd_socket(int fd, int *flags)
{
    struct open_file *f;
    struct posix_socket *s = NULL;

    pthread_mutex_lock(&lock);
    f = file_of(fd);

    if (f != NULL && f->sock != NULL) {
        s = f->sock;

        if (flags != NULL) {
            *flags = f->flags;
        }
    }

    pthread_mutex_unlock(&lock);
    return s;
}

int posix_fd_pipe_new(struct posix_pipe *p, int end, int flags)
{
    struct open_file *f = calloc(1, sizeof(*f));
    int fd;

    if (f == NULL) {
        return fails(ENOMEM);
    }

    f->refs = 1;
    f->flags = (end ? O_WRONLY : O_RDONLY) | (flags & O_NONBLOCK);
    f->pipe = p;
    f->pipe_end = end;

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

struct posix_pipe *posix_fd_pipe(int fd, int *flags, int *end)
{
    struct open_file *f;
    struct posix_pipe *p = NULL;

    pthread_mutex_lock(&lock);
    f = file_of(fd);

    if (f != NULL && f->pipe != NULL) {
        p = f->pipe;

        if (flags != NULL) {
            *flags = f->flags;
        }

        if (end != NULL) {
            *end = f->pipe_end;
        }
    }

    pthread_mutex_unlock(&lock);
    return p;
}

/* An open file whose last descriptor has gone, with the lock not held:
 * whatever is behind it let go of, and it freed. */
static void release(struct open_file *f)
{
    if (f == NULL) {
        return;
    }

    if (f->sock != NULL) {
        posix_socket_close(f->sock);
    }

    if (f->pipe != NULL) {
        posix_pipe_close(f->pipe, f->pipe_end);
    }

    free(f);
}

int posix_fd_open(int fd)
{
    int open_now;

    if (fd >= 0 && fd < CONSOLE) {
        return 1;
    }

    pthread_mutex_lock(&lock);
    open_now = file_of(fd) != NULL;
    pthread_mutex_unlock(&lock);
    return open_now;
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

    /* Zeroed, so a field added later - `sock` was - starts as nothing. */
    if ((f = calloc(1, sizeof(*f))) == NULL) {
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

/*
 * **Unlinked while open** (`docs/w5.md` W5c). POSIX keeps a file whose
 * name was removed for as long as a descriptor holds it, and ported code
 * counts on that: SQLite opens each temporary file and unlinks it at once,
 * and went on writing to a name the server had deleted - `VACUUM`'s
 * "disk I/O error". A descriptor here is a path, not an inode, so an
 * `unlink` of a path this process has open marks it instead, and the last
 * close removes it. What differs from POSIX, and is said so: until then
 * the name is still there for anyone else to see, and a process that
 * ends without closing leaves it.
 */

/* Whether another open file in this process has `f`'s path. With the lock
 * held; `f` itself already taken out of the table. */
static int open_elsewhere(const struct open_file *f)
{
    for (int fd = CONSOLE; fd < room; fd++) {
        struct open_file *g = slots[fd].file;

        if (g != NULL && g != f && g->sock == NULL && strcmp(g->path, f->path) == 0) {
            return 1;
        }
    }

    return 0;
}

/* Every open file with path `whole` marked to go at its last close;
 * whether there was one. */
static int unlink_when_closed(const char *whole)
{
    int marked = 0;

    pthread_mutex_lock(&lock);

    for (int fd = CONSOLE; fd < room; fd++) {
        struct open_file *g = slots[fd].file;

        if (g != NULL && g->sock == NULL && !g->dir && strcmp(g->path, whole) == 0) {
            g->unlinked = 1;
            marked = 1;
        }
    }

    pthread_mutex_unlock(&lock);
    return marked;
}

int close(int fd)
{
    struct open_file *f;

    if (fd >= 0 && fd < CONSOLE) {
        return 0;                       /* the console stays */
    }

    pthread_mutex_lock(&lock);
    f = file_of(fd);

    int remove_now = 0;

    if (f != NULL) {
        slots[fd].file = NULL;

        if (--f->refs > 0) {
            f = NULL;                   /* another descriptor still has it */
        } else {
            f->refs = -1;
            remove_now = f->unlinked && !open_elsewhere(f);
        }
    } else {
        fd = -1;
    }

    pthread_mutex_unlock(&lock);

    if (fd < 0) {
        return fails(EBADF);
    }

    if (remove_now) {
        (void)storage_delete(f->path);
    }

    release(f);
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
                release(old);
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

    /* Record locks: the third argument is a `struct flock *`, and there
     * are none to take (`fcntl.h`). */
    if (command == F_GETLK || command == F_SETLK || command == F_SETLKW) {
        struct taken t;

        return take(fd, &t) != 0 ? -1 : fails(ENOTSUP);
    }

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

/*
 * A console this process was handed (`posix_console`), or -1. A process
 * with no Lua side has nobody to collect its spill ring: a web process
 * WebKit started said nothing at all, its crash included (`docs/w7.md`
 * W7a). Handed the console's endpoint, it is heard as a server is - a
 * `CON_OP_WRITE` per piece, as `say` sends a line.
 */
static long handed_console = -1;

void posix_console(long capability)
{
    handed_console = capability;
}

long posix_console_capability(void)
{
    return handed_console;
}

static bool console_say(const char *s, size_t bytes)
{
    while (bytes > 0) {
        struct message msg, rep;
        struct con_request *req = (struct con_request *)msg.data;
        size_t n = bytes < CON_TEXT_MAX ? bytes : CON_TEXT_MAX;

        memset(&msg, 0, sizeof(msg));
        msg.length = sizeof(*req);
        req->op = CON_OP_WRITE;
        req->length = (uint32_t)n;
        memcpy(req->text, s, n);
        if (kosmos_call(handed_console, &msg, &rep) < 0) {
            return false;
        }
        s += n;
        bytes -= n;
    }

    return true;
}

/*
 * The console, or the spill ring when the console will not take it - as
 * the C library's `printf` does. `SYS_WRITE` is the console's owner's
 * alone, so for every other process this used to be EIO: SQLite's
 * `fuzzcheck`, run from the prompt, said nothing at all (`docs/w5.md`
 * W5c). Spilled, it is collected by whoever can reach a console - or,
 * where this process was handed one, it goes there.
 */
static ssize_t console_write(const void *buf, size_t bytes)
{
    if (bytes > 0 && kosmos_write(buf, bytes) < 0
        && !(handed_console >= 0 && console_say(buf, bytes))) {
        kosmos_spill(buf, bytes);
    }

    return (ssize_t)bytes;
}

/* A process's last words - `panic()` and Lua's (`user/init/`) - by the
 * same road as standard error, so a program that is not the console's
 * owner is heard as it ends rather than spilling into a ring nobody reads. */
void posix_last_words(const char *s, unsigned long bytes)
{
    (void)console_write(s, (size_t)bytes);
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
    struct posix_socket *s;
    struct taken t;
    ssize_t n;
    int flags;

    if (fd == 0) {
        return 0;
    }

    if (fd > 0 && fd < CONSOLE) {
        return fails(EBADF);
    }

    if ((s = posix_fd_socket(fd, &flags)) != NULL) {
        return posix_socket_recv(s, buf, bytes, (flags & O_NONBLOCK) != 0);
    }

    {
        int end;
        struct posix_pipe *p = posix_fd_pipe(fd, &flags, &end);

        if (p != NULL) {
            return end ? fails(EBADF) : posix_pipe_read(p, buf, bytes, (flags & O_NONBLOCK) != 0);
        }
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

    /* Over the file where it already is: a database's pages rewritten in
     * place, not into new blocks each time (`DISK_OVER`). */
    r = storage_write(t->path, at, buf, bytes, STORAGE_OVER);
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
    struct posix_socket *s;
    struct taken t;
    uint64_t at;
    ssize_t n;
    int flags;

    if (fd == 1 || fd == 2) {
        return console_write(buf, bytes);
    }

    if (fd == 0) {
        return fails(EBADF);
    }

    if ((s = posix_fd_socket(fd, &flags)) != NULL) {
        return posix_socket_send(s, buf, bytes, (flags & O_NONBLOCK) != 0);
    }

    {
        int end;
        struct posix_pipe *p = posix_fd_pipe(fd, &flags, &end);

        if (p != NULL) {
            return !end ? fails(EBADF) : posix_pipe_write(p, buf, bytes, (flags & O_NONBLOCK) != 0);
        }
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

    r = storage_flush(t.path);
    io_note(t.path, IO_SYNC, 0, r);
    return r == STORAGE_OK ? 0 : kit_fails(r);
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

    if (posix_fd_pipe(fd, NULL, NULL) != NULL) {
        memset(out, 0, sizeof(*out));
        out->st_mode = S_IFIFO | 0600;
        out->st_nlink = 1;
        out->st_blksize = 4096;
        return 0;
    }

    if (posix_fd_socket(fd, NULL) != NULL) {
        memset(out, 0, sizeof(*out));
        out->st_mode = S_IFSOCK | 0777;
        out->st_nlink = 1;
        out->st_blksize = 4096;
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

/* A file has no owner here, as it has no mode: what `fchmod` does. */
int fchown(int fd, uid_t owner, gid_t group)
{
    struct taken t;

    (void)owner;
    (void)group;
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
    char whole[STORAGE_PATH_MAX];
    int r;

    if ((r = storage_absolute(path, whole)) != STORAGE_OK
        || (r = storage_stat(whole, &st)) != STORAGE_OK) {
        return kit_fails(r);
    }

    if (st.kind == STORAGE_KIND_DIR) {
        return fails(EISDIR);
    }

    if (unlink_when_closed(whole)) {
        return 0;                       /* at the last close (above) */
    }

    return (r = storage_delete(whole)) == STORAGE_OK ? 0 : kit_fails(r);
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
    struct storage_stat st;
    int r;

    /* A file as `unlink` takes it, so an open one stays until closed. */
    if (storage_stat(path, &st) == STORAGE_OK && st.kind == STORAGE_KIND_FILE) {
        return unlink(path);
    }

    r = storage_delete(path);
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
 * Relative to a folder: the `at` calls. A descriptor here remembers the
 * path it opened, so a name relative to one is that path and the name -
 * resolved once, as the call is made, which is what POSIX asks of the
 * descriptor's folder and all a port leans on (libc++'s `remove_all`).
 */

static int at(int dirfd, const char *path, char whole[STORAGE_PATH_MAX])
{
    struct taken t;
    char joined[STORAGE_PATH_MAX];
    int r;

    if (path == NULL) {
        return fails(EFAULT);
    }

    if (path[0] == '/' || dirfd == AT_FDCWD) {
        r = storage_absolute(path, whole);
        return r == STORAGE_OK ? 0 : kit_fails(r);
    }

    if (take(dirfd, &t) != 0) {
        return -1;
    }

    if (!t.dir) {
        return fails(ENOTDIR);
    }

    if (snprintf(joined, sizeof(joined), "%s/%s", t.path, path) >= (int)sizeof(joined)) {
        return fails(ENAMETOOLONG);
    }

    r = storage_absolute(joined, whole);
    return r == STORAGE_OK ? 0 : kit_fails(r);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    char whole[STORAGE_PATH_MAX];
    mode_t mode = 0;

    if (flags & O_CREAT) {
        va_list ap;

        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    return at(dirfd, path, whole) != 0 ? -1 : open(whole, flags, mode);
}

int unlinkat(int dirfd, const char *path, int flags)
{
    char whole[STORAGE_PATH_MAX];

    if (flags & ~AT_REMOVEDIR) {
        return fails(EINVAL);
    }

    if (at(dirfd, path, whole) != 0) {
        return -1;
    }

    return flags & AT_REMOVEDIR ? rmdir(whole) : unlink(whole);
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
    char whole[STORAGE_PATH_MAX];

    (void)flags;                        /* no links, so nothing to follow */
    return at(dirfd, path, whole) != 0 ? -1 : chmod(whole, mode);
}

/*
 * **`sendfile`, Linux's: up to `count` bytes from `in` to `out`**, from
 * `*offset` when given - which is moved on and `in`'s own offset is not -
 * or else from `in`'s offset, which is. Through this process's memory a
 * piece at a time, since both ends are the Storage Kit's here; libc++'s
 * `copy_file` is what asks, as its way to copy without iostreams. The
 * count written, 0 at the end of `in`, or -1.
 */
ssize_t sendfile(int out, int in, off_t *offset, size_t count)
{
    enum { PIECE = 64 * 1024 };
    char *buf;
    size_t done = 0;

    if ((buf = malloc(count < PIECE ? (count ? count : 1) : PIECE)) == NULL) {
        return fails(ENOMEM);
    }

    while (done < count) {
        size_t want = count - done < PIECE ? count - done : PIECE;
        ssize_t got = offset != NULL ? pread(in, buf, want, *offset + (off_t)done)
                                     : read(in, buf, want);
        ssize_t put = 0;

        if (got <= 0) {
            if (got < 0 && done == 0) {
                free(buf);
                return -1;
            }
            break;
        }

        while (put < got) {
            ssize_t w = write(out, buf + put, (size_t)(got - put));

            if (w < 0) {
                free(buf);
                if (offset != NULL) {
                    *offset += (off_t)(done + (size_t)put);
                }
                return done + (size_t)put > 0 ? (ssize_t)(done + (size_t)put) : -1;
            }
            put += w;
        }

        done += (size_t)got;
    }

    free(buf);

    if (offset != NULL) {
        *offset += (off_t)done;
    }

    return (ssize_t)done;
}

/* No locks: a file is its server's, and they keep none (`sys/file.h`). */
int flock(int fd, int operation)
{
    struct taken t;

    (void)operation;
    return take(fd, &t) != 0 ? -1 : fails(ENOTSUP);
}

/*
 * **A file's times are the server's to keep.** `storage_stat` reports
 * when a file was last written, and nothing in the Storage Kit sets it:
 * the server stamps a write as it lands. So these say ENOTSUP rather
 * than pretend - a port that asked to backdate a file learns that it was
 * not done.
 */
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
    struct stat st;
    char whole[STORAGE_PATH_MAX];

    (void)times;
    (void)flags;

    if (at(dirfd, path, whole) != 0 || stat(whole, &st) != 0) {
        return -1;
    }

    return fails(ENOTSUP);
}

int futimens(int fd, const struct timespec times[2])
{
    struct taken t;

    (void)times;
    return take(fd, &t) != 0 ? -1 : fails(ENOTSUP);
}

int utimes(const char *path, const struct timeval times[2])
{
    struct stat st;

    (void)times;
    return stat(path, &st) != 0 ? -1 : fails(ENOTSUP);
}

/*
 * **There are no links.** No server here keeps one (`lstat` is `stat`,
 * and `O_NOFOLLOW` has nothing to refuse), so a path is never a link to
 * read - EINVAL, which is what POSIX says of a path that is not one - and
 * no link can be made: EPERM for a hard one, as POSIX says of a file
 * system that does not support them, and ENOTSUP for a symbolic one.
 */
ssize_t readlink(const char *path, char *buf, size_t room)
{
    struct stat st;

    (void)buf;
    (void)room;
    return stat(path, &st) != 0 ? -1 : fails(EINVAL);
}

int symlink(const char *target, const char *path)
{
    (void)target;
    (void)path;
    return fails(ENOTSUP);
}

int link(const char *from, const char *to)
{
    (void)from;
    (void)to;
    return fails(EPERM);
}

/*
 * **How much room a file system has** (16 November 2026): the Storage
 * Kit's `storage_room`, `DISK_OP_ROOM` on `/Home`, in blocks of 4 KB. It
 * said ENOTSUP before, and WebKit, sizing its disk cache from the free
 * space, gave the cache none - every visit fetched every file again. A
 * mount that cannot say - `/Temporary`, `/Kosmos` - is still ENOTSUP.
 */
static int room_of(const char *path, struct statvfs *out)
{
    const unsigned long block = 4096;
    uint64_t size, free_bytes;
    int r = storage_room(path, &size, &free_bytes);

    if (r == STORAGE_ERR_BAD) {
        return fails(ENOTSUP);
    }

    if (r != STORAGE_OK) {
        return kit_fails(r);
    }

    memset(out, 0, sizeof *out);
    out->f_bsize = block;
    out->f_frsize = block;
    out->f_blocks = (fsblkcnt_t)(size / block);
    out->f_bfree = (fsblkcnt_t)(free_bytes / block);
    out->f_bavail = out->f_bfree;
    out->f_namemax = 255;
    return 0;
}

int statvfs(const char *path, struct statvfs *out)
{
    char whole[STORAGE_PATH_MAX];
    int r;

    if (path == NULL || out == NULL) {
        return fails(EFAULT);
    }

    if ((r = storage_absolute(path, whole)) != STORAGE_OK) {
        return kit_fails(r);
    }

    return room_of(whole, out);
}

int fstatvfs(int fd, struct statvfs *out)
{
    struct taken t;

    if (out == NULL) {
        return fails(EFAULT);
    }

    if (take(fd, &t) != 0) {
        return -1;
    }

    return room_of(t.path, out);
}

long pathconf(const char *path, int name)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return -1;
    }

    switch (name) {
    case _PC_NAME_MAX: return 255;
    case _PC_PATH_MAX: return STORAGE_PATH_MAX;
    default:           return fails(EINVAL);
    }
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

/*
 * **The working directory is the one the process was started in.** The
 * Storage Kit keeps it, read by every call that resolves a name and set
 * once (`storage_start`); changing it under threads that are resolving
 * would want a lock on every path, for a call ported code makes almost
 * never. So `chdir` to where the process already is succeeds, and
 * anywhere else is ENOTSUP (`roadmap.md`, W4c's follow-ups).
 */
int chdir(const char *path)
{
    struct storage_stat st;
    char whole[STORAGE_PATH_MAX];
    int r;

    if (path == NULL) {
        return fails(EFAULT);
    }

    if ((r = storage_absolute(path, whole)) != STORAGE_OK
        || (r = storage_stat(whole, &st)) != STORAGE_OK) {
        return kit_fails(r);
    }

    if (st.kind != STORAGE_KIND_DIR) {
        return fails(ENOTDIR);
    }

    return strcmp(whole, storage_cwd()) == 0 ? 0 : fails(ENOTSUP);
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
    int           fd;                   /* fdopendir's, closed with it; -1 */
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

    d->fd = -1;
    return d;
}

/* The folder a descriptor opened, as a stream; the descriptor is the
 * stream's from now on, and `closedir` closes it. */
DIR *fdopendir(int fd)
{
    struct taken t;
    DIR *d;

    if (take(fd, &t) != 0) {
        return NULL;
    }

    if (!t.dir) {
        errno = ENOTDIR;
        return NULL;
    }

    if ((d = opendir(t.path)) != NULL) {
        d->fd = fd;
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

    if (d->fd >= 0) {
        close(d->fd);
    }

    free(d);
    return 0;
}
