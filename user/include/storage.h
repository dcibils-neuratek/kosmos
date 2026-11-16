/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_STORAGE_H
#define KOSMOS_STORAGE_H

/*
 * **The Storage Kit: files, from C, from any thread** (`docs/w3.md` W3d).
 *
 * The client side of the file servers' protocols - `diskproto.h` for
 * `/Home` (through `diskask`, the one C door to it), `ramproto.h` for
 * `/Temporary`, `binproto.h` for `/Kosmos` - by path, since no server hands
 * out a handle. What a process may reach is the mount table its runner
 * hands over once, with `storage_start`, before the program runs: each
 * prefix, its capability, its root on the server and its protocol. Nothing
 * else is reachable, and a path that matches no mount is
 * STORAGE_ERR_NO_MOUNT - the namespace's rule, from C.
 *
 * **Any thread.** The table is written once and only read after; every call
 * builds its own messages; the servers answer one request at a time. So two
 * threads may read and write at once, as a ported library's do.
 *
 * Errors are the kit's own, negative, and say what happened in Kosmos's
 * terms; `runtime/posix/` turns them into `errno`.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_DISK 1u                 /* diskproto */
#define STORAGE_RAM  2u                 /* ramproto */
#define STORAGE_BIN  3u                 /* binproto, read only */

#define STORAGE_PREFIX_MAX 64
#define STORAGE_ROOT_MAX   64
#define STORAGE_PATH_MAX   512          /* a whole path, as `/Home`'s */
#define STORAGE_MOUNTS_MAX 32

struct storage_mount {
    char     prefix[STORAGE_PREFIX_MAX];    /* "/Home" */
    char     root[STORAGE_ROOT_MAX];        /* on the server: "/Home", "/apps", "" */
    long     cap;                           /* this process's index for it */
    uint32_t proto;                         /* STORAGE_DISK, _RAM, _BIN */
};

#define STORAGE_OK              0
#define STORAGE_ERR_NO_MOUNT   -1   /* nothing is mounted where the path goes */
#define STORAGE_ERR_NO_FILE    -2
#define STORAGE_ERR_NOT_DIR    -3
#define STORAGE_ERR_IS_DIR     -4
#define STORAGE_ERR_EXISTS     -5
#define STORAGE_ERR_NOT_EMPTY  -6
#define STORAGE_ERR_FULL       -7
#define STORAGE_ERR_READ_ONLY  -8
#define STORAGE_ERR_DENIED     -9
#define STORAGE_ERR_CROSSES   -10   /* a rename from one mount to another */
#define STORAGE_ERR_TOO_LONG  -11   /* longer than the server takes */
#define STORAGE_ERR_IO        -12   /* the server did not answer, or said so */
#define STORAGE_ERR_BAD       -13   /* what was asked makes no sense */
#define STORAGE_ERR_NOT_READY -14   /* no table handed over yet */

/* The mount table and the working directory, once; a second call is
 * STORAGE_ERR_DENIED, so a program cannot hand itself another. */
int storage_start(const struct storage_mount *mounts, unsigned count, const char *cwd);

/* The mount table that was handed over, up to `max` of it, and how many
 * there are: for a process starting another to hand on what it may reach,
 * or less (`docs/w7.md` W7a's launcher). Reading it changes nothing, and
 * the table cannot be handed over again. */
unsigned storage_mounts(struct storage_mount *out, unsigned max);

/* The working directory the process was started in. */
const char *storage_cwd(void);

/* `path` made whole: relative to the working directory, `.` and `..` and
 * doubled slashes gone. STORAGE_OK, or STORAGE_ERR_TOO_LONG. */
int storage_absolute(const char *path, char out[STORAGE_PATH_MAX]);

#define STORAGE_KIND_FILE 1u
#define STORAGE_KIND_DIR  2u

struct storage_stat {
    uint32_t kind;                  /* STORAGE_KIND_* */
    uint32_t writable;              /* 0 on a read-only mount */
    uint64_t size;
    uint64_t modified;              /* seconds since 1970, or 0: not known */
};

int  storage_stat(const char *path, struct storage_stat *out);

/* Up to `bytes` from `offset`: how many, 0 at the end, or an error. */
long storage_read(const char *path, uint64_t offset, void *buf, size_t bytes);

#define STORAGE_CREATE 1u               /* made if it is not there */
#define STORAGE_CUT    2u               /* and it ends where these bytes end */
/* Bytes inside the file laid over its blocks rather than written anew:
 * POSIX's `write` (`DISK_OVER`, `testing.md` 18.836). */
#define STORAGE_OVER   4u

/* `bytes` at `offset`; without STORAGE_CREATE a file that is not there is
 * STORAGE_ERR_NO_FILE. How many were written, or an error. */
long storage_write(const char *path, uint64_t offset, const void *buf, size_t bytes,
                   unsigned flags);

/* The file made `size` long, shorter or longer, the rest zeros. */
int  storage_truncate(const char *path, uint64_t size);

/*
 * A directory's names, a page at a time: from `*cursor` (0 to begin), each
 * ending in a zero byte, as many as fit in `room`. How many names, 0 at the
 * end; `*cursor` is where the next page starts.
 */
long storage_list(const char *path, uint64_t *cursor, char *names, size_t room);

int  storage_mkdir(const char *path);
int  storage_delete(const char *path);              /* a file, or an empty directory */
int  storage_rename(const char *from, const char *to);
int  storage_flush(const char *path);               /* to the device, where there is one */

/* The room on the filesystem `path` is on: its bytes and its free bytes
 * (`DISK_OP_ROOM`). STORAGE_ERR_BAD where a mount cannot say - `/Temporary`
 * and `/Kosmos` - or its server does not know the request. */
int  storage_room(const char *path, uint64_t *size, uint64_t *free_bytes);

#ifdef __cplusplus
}
#endif

#endif /* KOSMOS_STORAGE_H */
