/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Storage Kit** (`storage.h`, `docs/w3.md` W3d): a path resolved
 * against the mount table its runner handed over, and asked of that mount's
 * server in its own protocol.
 *
 * Three protocols, and the differences are the servers', not this file's:
 *
 *   `/Home`      diskproto, through `diskask` - the one C door to it. Read
 *                and written at an offset: a few kilobytes in the message,
 *                and more through a region of the call's own, a megabyte a
 *                request (`PIECE_BYTES`); `WRITE_AT` with `DISK_CREATE` and
 *                `DISK_CUT` is POSIX's write and truncate. Names page by
 *                index.
 *   `/Temporary` ramproto. A write always makes the file, so "not there"
 *                is asked first when it must not; `RAM_KEEP` keeps what is
 *                there, `RAM_CUT` ends it. Four names a page.
 *   `/Kosmos`    binproto, read only: 1632 bytes a message, and a path of
 *                63 bytes with its root.
 *
 * **Nothing here is shared between calls but the table**, which is copied
 * in once by `storage_start` and published with a release store; every
 * call's messages are on its own stack. So any thread may call any of it at
 * once.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "binproto.h"
#include "diskask.h"
#include "diskproto.h"
#include "kosmos.h"
#include "ramask.h"
#include "ramproto.h"
#include "storage.h"

static struct storage_mount table[STORAGE_MOUNTS_MAX];
static unsigned mounted;
static char cwd[STORAGE_PATH_MAX] = "/";
static volatile uint32_t ready;         /* 1 once the table is in */
static volatile uint32_t taken;         /* 1 once somebody began handing it over */

int storage_start(const struct storage_mount *mounts, unsigned count, const char *dir)
{
    unsigned i;

    if (__atomic_exchange_n(&taken, 1u, __ATOMIC_ACQ_REL) != 0) {
        return STORAGE_ERR_DENIED;      /* once, by the runner */
    }

    if (count > STORAGE_MOUNTS_MAX) {
        count = STORAGE_MOUNTS_MAX;
    }

    for (i = 0; i < count; i++) {
        table[i] = mounts[i];
        table[i].prefix[STORAGE_PREFIX_MAX - 1] = '\0';
        table[i].root[STORAGE_ROOT_MAX - 1] = '\0';
    }

    mounted = count;

    if (dir != NULL && dir[0] == '/' && strlen(dir) < sizeof(cwd)) {
        strcpy(cwd, dir);
    }

    __atomic_store_n(&ready, 1u, __ATOMIC_RELEASE);
    return STORAGE_OK;
}

unsigned storage_mounts(struct storage_mount *out, unsigned max)
{
    unsigned i;

    if (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) == 0) {
        return 0;
    }

    for (i = 0; i < mounted && i < max; i++) {
        out[i] = table[i];
    }

    return mounted;
}

const char *storage_cwd(void)
{
    return cwd;
}

/*
 *--------------------------------------------------------------------------
 * Paths.
 */

int storage_absolute(const char *path, char out[STORAGE_PATH_MAX])
{
    char whole[STORAGE_PATH_MAX * 2];
    size_t n = 0;
    const char *p;

    if (path == NULL || path[0] == '\0') {
        return STORAGE_ERR_NO_FILE;
    }

    if (path[0] == '/') {
        if (strlen(path) >= sizeof(whole)) {
            return STORAGE_ERR_TOO_LONG;
        }

        strcpy(whole, path);
    } else if (snprintf(whole, sizeof(whole), "%s/%s", cwd, path) >= (int)sizeof(whole)) {
        return STORAGE_ERR_TOO_LONG;
    }

    /* Component by component: `.` dropped, `..` takes the last one off,
     * and a slash between each. */
    out[0] = '\0';
    p = whole;

    while (*p != '\0') {
        const char *start;
        size_t len;

        while (*p == '/') {
            p++;
        }

        start = p;

        while (*p != '\0' && *p != '/') {
            p++;
        }

        len = (size_t)(p - start);

        if (len == 0 || (len == 1 && start[0] == '.')) {
            continue;
        }

        if (len == 2 && start[0] == '.' && start[1] == '.') {
            while (n > 0 && out[n - 1] != '/') {
                n--;
            }

            if (n > 0) {
                n--;                    /* the slash before it too */
            }

            out[n] = '\0';
            continue;
        }

        if (n + 1 + len >= STORAGE_PATH_MAX) {
            return STORAGE_ERR_TOO_LONG;
        }

        out[n++] = '/';
        memcpy(out + n, start, len);
        n += len;
        out[n] = '\0';
    }

    if (n == 0) {
        strcpy(out, "/");
    }

    return STORAGE_OK;
}

/* strnlen, which this libc does not have. */
static size_t span(const char *s, size_t most)
{
    size_t n = 0;

    while (n < most && s[n] != '\0') {
        n++;
    }

    return n;
}

/* Where a path goes: its mount, and the path on that mount's server. */
struct where {
    const struct storage_mount *m;
    char path[STORAGE_PATH_MAX];
};

static int resolve(const char *path, struct where *w)
{
    char whole[STORAGE_PATH_MAX];
    const struct storage_mount *best = NULL;
    size_t best_len = 0, plen;
    const char *rest;
    unsigned i;
    int r;

    if (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) == 0) {
        return STORAGE_ERR_NOT_READY;
    }

    if ((r = storage_absolute(path, whole)) != STORAGE_OK) {
        return r;
    }

    /* The longest prefix that ends at a slash or at the end. */
    for (i = 0; i < mounted; i++) {
        size_t len = strlen(table[i].prefix);

        if (len > best_len && strncasecmp(whole, table[i].prefix, len) == 0
            && (whole[len] == '\0' || whole[len] == '/')) {
            best = &table[i];
            best_len = len;
        }
    }

    if (best == NULL) {
        return STORAGE_ERR_NO_MOUNT;
    }

    rest = whole + best_len;
    plen = strlen(best->root) + strlen(rest);

    if (plen >= sizeof(w->path)
        || (best->proto == STORAGE_RAM && plen >= RAM_PATH_MAX)
        || (best->proto == STORAGE_BIN && plen >= BIN_NAME_MAX)) {
        return STORAGE_ERR_TOO_LONG;
    }

    snprintf(w->path, sizeof(w->path), "%s%s", best->root, rest);

    if (w->path[0] == '\0') {
        strcpy(w->path, "/");
    }

    w->m = best;
    return STORAGE_OK;
}

/*
 *--------------------------------------------------------------------------
 * `/Home`: diskproto, through diskask.
 */

/* kfs's own numbers, as `diskfs` hands them on: DISK_ERR_KFS + -KFS_E_*. */
#define KFS_NO_FILE   7u
#define KFS_NOT_DIR   8u
#define KFS_IS_DIR    9u
#define KFS_TAKEN    10u
#define KFS_NOT_EMPTY 11u
#define KFS_FULL     12u
#define KFS_NO_INODES 13u
#define KFS_DOTS     18u

static int disk_error(uint32_t e)
{
    switch (e) {
    case DISK_OK:                       return STORAGE_OK;
    case DISK_ERR_KFS + KFS_NO_FILE:    return STORAGE_ERR_NO_FILE;
    case DISK_ERR_KFS + KFS_NOT_DIR:    return STORAGE_ERR_NOT_DIR;
    case DISK_ERR_KFS + KFS_IS_DIR:     return STORAGE_ERR_IS_DIR;
    case DISK_ERR_THE_DIRECTORY:        return STORAGE_ERR_IS_DIR;
    case DISK_ERR_KFS + KFS_TAKEN:      return STORAGE_ERR_EXISTS;
    case DISK_ERR_KFS + KFS_NOT_EMPTY:  return STORAGE_ERR_NOT_EMPTY;
    case DISK_ERR_KFS + KFS_FULL:
    case DISK_ERR_KFS + KFS_NO_INODES:  return STORAGE_ERR_FULL;
    case DISK_ERR_KFS + KFS_DOTS:       return STORAGE_ERR_BAD;
    case DISK_ERR_READ_ONLY:            return STORAGE_ERR_READ_ONLY;
    case DISK_ERR_DENIED:               return STORAGE_ERR_DENIED;
    default:                            return STORAGE_ERR_IO;
    }
}

static struct diskask *disk_door(const struct where *w)
{
    struct diskask *d = malloc(sizeof(*d));

    if (d != NULL) {
        d->ep = w->m->cap;
    }

    return d;
}

static int disk_stat(const struct where *w, struct storage_stat *out)
{
    struct diskask *d = disk_door(w);
    const struct disk_reply *rp;
    uint32_t e;

    if (d == NULL) {
        return STORAGE_ERR_FULL;
    }

    e = diskask(d, DISK_OP_GETATTR, w->path, 0, 0, 0, -1, NULL, 0);
    rp = diskask_said(d);

    if (e == DISK_OK) {
        out->kind = rp->node.kind == DISK_KIND_FILE ? STORAGE_KIND_FILE : STORAGE_KIND_DIR;
        out->size = out->kind == STORAGE_KIND_FILE ? rp->node.size : 0;
        out->modified = rp->node.dated ? rp->node.modified : 0;
        out->writable = 1;
    }

    free(d);
    return disk_error(e);
}

/*
 * **A read or a write of more than a few kilobytes goes through a region**
 * (16 November 2026). Every `/Home` read here was a kilobyte a message, so
 * WebKit finding its three compiled blocking lists - 67 MB - was some
 * 68,000 round trips to `diskfs` and 3.8 s on the M700 before JetSki's
 * first page could load (`testing.md` 18.841), where `diskfs` reads a
 * region of any size in one request, as `smbd` and the LLM server ask it.
 *
 * The region is the call's own - made, used and given back inside it -
 * because nothing here is shared between calls and any thread may be in
 * this file at once (above); one shared region would need a lock and would
 * put every thread's reads behind each other's. A megabyte a request at
 * most, so a call of any size holds a bounded region.
 */
#define PIECE_BYTES  (1024u * 1024u)
#define PIECE_LEAST  (4u * DISK_DATA_MAX)  /* below it, messages are cheaper */

struct piece {
    long cap;
    uint8_t *at;
    unsigned long pages;
    size_t room;
};

/* A region for `bytes`, a megabyte at most; false when none could be had,
 * and the caller goes on a kilobyte a message as before. */
static bool piece_open(struct piece *p, size_t bytes)
{
    long at;

    p->room = bytes < PIECE_BYTES ? bytes : PIECE_BYTES;
    p->pages = (p->room + 4095u) / 4096u;
    p->cap = kosmos_mem_create(p->pages);

    if (p->cap < 0) {
        return false;
    }

    at = kosmos_mem_map(p->cap);

    if (at < 0) {
        kosmos_cap_drop(p->cap);
        return false;
    }

    p->at = (uint8_t *)(uintptr_t)at;
    return true;
}

static void piece_close(struct piece *p)
{
    kosmos_share_unmap((unsigned long)(uintptr_t)p->at, p->pages);
    kosmos_cap_drop(p->cap);
}

/* `bytes` from `offset`, a megabyte a request into a region: how many were
 * read, an error, or STORAGE_ERR_FULL with nothing done when there was no
 * region to be had. */
static long disk_read_pieces(struct diskask *d, const struct where *w, uint64_t offset,
                             uint8_t *buf, size_t bytes)
{
    struct piece p;
    size_t got = 0;
    long r = 0;

    if (!piece_open(&p, bytes)) {
        return STORAGE_ERR_FULL;
    }

    while (got < bytes) {
        size_t want = bytes - got > p.room ? p.room : bytes - got;
        uint32_t e = diskask(d, DISK_OP_READ, w->path, DISK_REGION, offset + got, want,
                             p.cap, NULL, 0);
        size_t n;

        if (e != DISK_OK) {
            r = disk_error(e);
            break;
        }

        n = diskask_said(d)->bytes;
        n = n < want ? n : want;
        memcpy(buf + got, p.at, n);
        got += n;

        if (n < want) {
            break;                      /* the end of the file */
        }
    }

    piece_close(&p);
    return got > 0 ? (long)got : r;
}

static long disk_read(const struct where *w, uint64_t offset, uint8_t *buf, size_t bytes)
{
    struct diskask *d = disk_door(w);
    size_t got = 0;
    long r = 0;

    if (d == NULL) {
        return STORAGE_ERR_FULL;
    }

    if (bytes >= PIECE_LEAST) {
        r = disk_read_pieces(d, w, offset, buf, bytes);

        if (r != STORAGE_ERR_FULL) {
            free(d);
            return r;
        }

        r = 0;
    }

    while (got < bytes) {
        size_t want = bytes - got > DISK_DATA_MAX ? DISK_DATA_MAX : bytes - got;
        uint32_t e = diskask(d, DISK_OP_READ, w->path, 0, offset + got, want, -1, NULL, 0);
        const struct disk_reply *rp = diskask_said(d);
        size_t n;

        if (e != DISK_OK) {
            r = disk_error(e);
            break;
        }

        n = rp->length < want ? rp->length : want;
        memcpy(buf + got, rp->u.data, n);
        got += n;

        if (n < want) {
            break;                      /* the end of the file */
        }
    }

    free(d);
    return got > 0 ? (long)got : r;
}

static long disk_write(const struct where *w, uint64_t offset, const uint8_t *buf,
                       size_t bytes, unsigned flags)
{
    struct diskask *d = disk_door(w);
    struct piece p = { -1, NULL, 0, 0 };
    bool pieces = false;
    size_t done = 0;
    long r = 0;

    if (d == NULL) {
        return STORAGE_ERR_FULL;
    }

    if (buf != NULL && bytes >= PIECE_LEAST && piece_open(&p, bytes)) {
        pieces = true;
    }

    do {
        size_t most = pieces ? p.room : DISK_DATA_MAX;
        size_t n = bytes - done > most ? most : bytes - done;
        uint32_t f = 0;
        uint32_t e;

        if (flags & STORAGE_CREATE) {
            f |= DISK_CREATE;
        }

        if ((flags & STORAGE_CUT) && done + n == bytes) {
            f |= DISK_CUT;              /* the last piece ends it */
        }

        if (flags & STORAGE_OVER) {
            f |= DISK_OVER;
        }

        if (pieces) {
            memcpy(p.at, buf + done, n);
            e = diskask(d, DISK_OP_WRITE_AT, w->path, f | DISK_REGION, offset + done, n,
                        p.cap, NULL, 0);
        } else {
            e = diskask(d, DISK_OP_WRITE_AT, w->path, f, offset + done, n, -1,
                        buf != NULL ? buf + done : NULL, (uint32_t)n);
        }

        if (e != DISK_OK) {
            r = disk_error(e);
            break;
        }

        done += n;
    } while (done < bytes);

    if (pieces) {
        piece_close(&p);
    }

    free(d);
    return r < 0 && done == 0 ? r : (long)done;
}

static long disk_list(const struct where *w, uint64_t *cursor, char *names, size_t room)
{
    struct diskask *d = disk_door(w);
    const struct disk_reply *rp;
    uint32_t e;
    long count = 0;
    size_t at = 0, i = 0;

    if (d == NULL) {
        return STORAGE_ERR_FULL;
    }

    e = diskask(d, DISK_OP_LIST, w->path, 0, *cursor, 0, -1, NULL, 0);
    rp = diskask_said(d);

    if (e != DISK_OK) {
        free(d);
        return disk_error(e);
    }

    /* As many of the page's names as fit; the cursor moves past only
     * those, so a short `room` is never a name lost. */
    while (i < rp->length && count < (long)rp->count) {
        size_t len = span(rp->u.data + i, rp->length - i);

        if (at + len + 1 > room) {
            break;
        }

        memcpy(names + at, rp->u.data + i, len);
        names[at + len] = '\0';
        at += len + 1;
        i += len + 1;
        count++;
    }

    *cursor += (uint64_t)count;
    free(d);
    return count;
}

static int disk_simple(const struct where *w, uint32_t op, const char *to)
{
    struct diskask *d = disk_door(w);
    uint32_t e;

    if (d == NULL) {
        return STORAGE_ERR_FULL;
    }

    e = diskask(d, op, w->path, 0, 0, 0, -1, to, to != NULL ? (uint32_t)strlen(to) : 0);
    free(d);
    return disk_error(e);
}

/*
 *--------------------------------------------------------------------------
 * `/Temporary`: ramproto.
 */

static int ram_error(uint32_t e)
{
    switch (e) {
    case RAM_OK:               return STORAGE_OK;
    case RAM_ERR_NO_PATH:      return STORAGE_ERR_NO_FILE;
    case RAM_ERR_NOT_DIR:      return STORAGE_ERR_NOT_DIR;
    case RAM_ERR_NOT_READABLE: return STORAGE_ERR_IS_DIR;
    case RAM_ERR_FULL:         return STORAGE_ERR_FULL;
    case RAM_ERR_NOT_EMPTY:    return STORAGE_ERR_NOT_EMPTY;
    case RAM_ERR_EXISTS:       return STORAGE_ERR_EXISTS;
    default:                   return STORAGE_ERR_IO;
    }
}

/* A door to the store, on the heap: a request and a reply are a message
 * each, and the asking is `ramask`'s, the one C client of `ramproto`. */
static struct ramask *ram_door(const struct where *w)
{
    struct ramask *x = malloc(sizeof(*x));

    if (x != NULL) {
        x->ep = w->m->cap;
    }

    return x;
}

#define ram_said ramask_said

static int ram_stat(const struct where *w, struct storage_stat *out)
{
    struct ramask *x = ram_door(w);
    uint32_t e, i;

    /* The root has no node of its own to ask about. */
    if (strcmp(w->path, "/") == 0) {
        free(x);
        out->kind = STORAGE_KIND_DIR;
        out->writable = 1;
        return STORAGE_OK;
    }

    if (x == NULL) {
        return STORAGE_ERR_FULL;
    }

    e = ramask(x, RAM_OP_GETATTR, w->path, 0, 0, 0, 0, 0, NULL, NULL, 0, -1);

    if (e == RAM_OK) {
        const struct ram_reply *rp = ram_said(x);

        out->kind = STORAGE_KIND_FILE;
        out->size = rp->size;
        out->modified = 0;              /* the ram server keeps no time */
        out->writable = 1;

        for (i = 0; i < rp->count && i < RAM_ATTRS_MAX; i++) {
            if (strcmp(rp->u.attrs[i].name, "kind") == 0
                && strcmp(rp->u.attrs[i].value, "directory") == 0) {
                out->kind = STORAGE_KIND_DIR;
            } else if (strcmp(rp->u.attrs[i].name, "size") == 0 && out->size == 0) {
                out->size = strtoull(rp->u.attrs[i].value, NULL, 10);
            }
        }

        if (out->kind == STORAGE_KIND_DIR) {
            out->size = 0;
        }
    }

    free(x);
    return e == RAMASK_NO_ANSWER ? STORAGE_ERR_IO : ram_error(e);
}

static long ram_read(const struct where *w, uint64_t offset, uint8_t *buf, size_t bytes)
{
    struct ramask *x = ram_door(w);
    size_t got = 0;
    long r = 0;

    if (x == NULL) {
        return STORAGE_ERR_FULL;
    }

    while (got < bytes) {
        size_t want = bytes - got > RAM_DATA_MAX ? RAM_DATA_MAX : bytes - got;
        uint32_t e;
        size_t n;

        if (offset + got > 0xffffffffu) {
            break;                      /* the ram server's offsets are 32 bits */
        }

        e = ramask(x, RAM_OP_READ, w->path, (uint32_t)(offset + got), (uint32_t)want, 0, 0, 0,
                   NULL, NULL, 0, -1);

        if (e != RAM_OK) {
            r = e == RAMASK_NO_ANSWER ? STORAGE_ERR_IO : ram_error(e);
            break;
        }

        n = ram_said(x)->length < want ? ram_said(x)->length : want;
        memcpy(buf + got, ram_said(x)->u.data, n);
        got += n;

        if (n < want) {
            break;
        }
    }

    free(x);
    return got > 0 ? (long)got : r;
}

static long ram_write(const struct where *w, uint64_t offset, const uint8_t *buf,
                      size_t bytes, unsigned flags)
{
    struct ramask *x;
    size_t done = 0;
    long r = 0;

    /* The ram server makes whatever it is asked to write, so "only if it
     * is there" is asked first; and a file inside a folder that is not
     * there is not made, as `/Home` would not. */
    {
        struct storage_stat st;
        int s = ram_stat(w, &st);

        if (s == STORAGE_OK && st.kind == STORAGE_KIND_DIR) {
            return STORAGE_ERR_IS_DIR;
        }

        if (s == STORAGE_ERR_NO_FILE) {
            struct where parent = *w;
            char *slash = strrchr(parent.path, '/');

            if (!(flags & STORAGE_CREATE)) {
                return STORAGE_ERR_NO_FILE;
            }

            if (slash != NULL && slash != parent.path) {
                *slash = '\0';

                if (ram_stat(&parent, &st) != STORAGE_OK || st.kind != STORAGE_KIND_DIR) {
                    return STORAGE_ERR_NO_FILE;
                }
            }
        } else if (s != STORAGE_OK) {
            return s;
        }
    }

    if (offset + bytes > 0xffffffffu) {
        return STORAGE_ERR_FULL;
    }

    x = ram_door(w);

    if (x == NULL) {
        return STORAGE_ERR_FULL;
    }

    do {
        size_t n = bytes - done > RAM_DATA_MAX ? RAM_DATA_MAX : bytes - done;
        uint32_t e = ramask(x, RAM_OP_WRITE, w->path, (uint32_t)(offset + done), (uint32_t)n, 0,
                            RAM_RAW,
                            RAM_KEEP | (((flags & STORAGE_CUT) && done + n == bytes) ? RAM_CUT : 0),
                            NULL, n > 0 ? buf + done : NULL, (uint32_t)n, -1);

        if (e != RAM_OK) {
            r = e == RAMASK_NO_ANSWER ? STORAGE_ERR_IO : ram_error(e);
            break;
        }

        done += n;
    } while (done < bytes);

    free(x);
    return r < 0 && done == 0 ? r : (long)done;
}

static long ram_list(const struct where *w, uint64_t *cursor, char *names, size_t room)
{
    struct ramask *x = ram_door(w);
    uint32_t e, i;
    long count = 0;
    size_t at = 0;

    if (x == NULL) {
        return STORAGE_ERR_FULL;
    }

    e = ramask(x, RAM_OP_LIST, w->path, (uint32_t)*cursor, 0, 0, 0, 0, NULL, NULL, 0, -1);

    if (e != RAM_OK) {
        free(x);
        return e == RAMASK_NO_ANSWER ? STORAGE_ERR_IO : ram_error(e);
    }

    for (i = 0; i < ram_said(x)->count && i < RAM_ENTRIES_MAX; i++) {
        const char *name = ram_said(x)->u.entries[i];
        size_t len = span(name, RAM_PATH_MAX);

        if (at + len + 1 > room) {
            break;
        }

        memcpy(names + at, name, len);
        names[at + len] = '\0';
        at += len + 1;
        count++;
    }

    *cursor += (uint64_t)count;
    free(x);
    return count;
}

static int ram_simple(const struct where *w, uint32_t op, const char *to)
{
    struct ramask *x = ram_door(w);
    uint32_t e;

    if (x == NULL) {
        return STORAGE_ERR_FULL;
    }

    e = ramask(x, op, w->path, 0, to != NULL ? (uint32_t)strlen(to) : 0, 0, 0, 0, NULL, to,
               to != NULL ? (uint32_t)strlen(to) + 1 : 0, -1);
    free(x);
    return e == RAMASK_NO_ANSWER ? STORAGE_ERR_IO : ram_error(e);
}

/*
 *--------------------------------------------------------------------------
 * `/Kosmos`: binproto, read only.
 */

struct bin_exchange {
    struct message msg, rep;
};

static uint32_t bin_ask(const struct where *w, struct bin_exchange *x, uint32_t op,
                        uint32_t offset)
{
    struct bin_request *rq = (struct bin_request *)(void *)x->msg.data;

    memset(&x->msg, 0, sizeof(x->msg));
    rq->op = op;
    rq->offset = offset;
    snprintf(rq->name, sizeof(rq->name), "%s", w->path);
    x->msg.length = sizeof(*rq);

    if (kosmos_call(w->m->cap, &x->msg, &x->rep) != 0
        || x->rep.length < offsetof(struct bin_reply, data)) {
        return 0xffffffffu;
    }

    return ((const struct bin_reply *)(const void *)x->rep.data)->error;
}

static const struct bin_reply *bin_said(const struct bin_exchange *x)
{
    return (const struct bin_reply *)(const void *)x->rep.data;
}

static int bin_error(uint32_t e)
{
    return e == BIN_OK ? STORAGE_OK
         : e == BIN_ERR_NO_PROGRAM ? STORAGE_ERR_NO_FILE
         : STORAGE_ERR_IO;
}

static int bin_stat(const struct where *w, struct storage_stat *out)
{
    struct bin_exchange *x;
    uint32_t e;

    out->modified = 0;
    out->writable = 0;

    /* The mount's own root - `/apps`, or the store's `/` - is a directory
     * the server has no entry for, so it is not asked. */
    if (strcmp(w->path, "/") == 0 || strcmp(w->path, w->m->root) == 0) {
        out->kind = STORAGE_KIND_DIR;
        out->size = 0;
        return STORAGE_OK;
    }

    if ((x = malloc(sizeof(*x))) == NULL) {
        return STORAGE_ERR_FULL;
    }

    e = bin_ask(w, x, BIN_OP_GETATTR, 0);

    if (e == BIN_OK) {
        bool dir = strcmp(bin_said(x)->kind, "directory") == 0;

        out->kind = dir ? STORAGE_KIND_DIR : STORAGE_KIND_FILE;
        out->size = dir ? 0 : bin_said(x)->size;
    }

    free(x);
    return e == 0xffffffffu ? STORAGE_ERR_IO : bin_error(e);
}

static long bin_read(const struct where *w, uint64_t offset, uint8_t *buf, size_t bytes)
{
    struct bin_exchange *x = malloc(sizeof(*x));
    size_t got = 0;
    long r = 0;

    if (x == NULL) {
        return STORAGE_ERR_FULL;
    }

    while (got < bytes && offset + got <= 0xffffffffu) {
        uint32_t e = bin_ask(w, x, BIN_OP_READ, (uint32_t)(offset + got));
        size_t n = bin_said(x)->length;

        if (e != BIN_OK) {
            r = e == 0xffffffffu ? STORAGE_ERR_IO : bin_error(e);
            break;
        }

        if (n > BIN_CHUNK) {
            n = BIN_CHUNK;
        }

        if (n > bytes - got) {
            n = bytes - got;
        }

        memcpy(buf + got, bin_said(x)->data, n);
        got += n;

        if (n == 0 || !bin_said(x)->more) {
            break;
        }
    }

    free(x);
    return got > 0 ? (long)got : r;
}

static long bin_list(const struct where *w, uint64_t *cursor, char *names, size_t room)
{
    struct bin_exchange *x = malloc(sizeof(*x));
    uint32_t e, i;
    long count = 0;
    size_t at = 0;

    if (x == NULL) {
        return STORAGE_ERR_FULL;
    }

    e = bin_ask(w, x, BIN_OP_LIST, (uint32_t)*cursor);

    if (e != BIN_OK) {
        free(x);
        return e == 0xffffffffu ? STORAGE_ERR_IO : bin_error(e);
    }

    for (i = 0; i < bin_said(x)->count && i < BIN_CHUNK / BIN_NAME_MAX; i++) {
        const char *name = (const char *)bin_said(x)->data + i * BIN_NAME_MAX;
        size_t len = span(name, BIN_NAME_MAX);

        if (at + len + 1 > room) {
            break;
        }

        memcpy(names + at, name, len);
        names[at + len] = '\0';
        at += len + 1;
        count++;
    }

    *cursor += (uint64_t)count;
    free(x);
    return count;
}

/*
 *--------------------------------------------------------------------------
 * The doors, by protocol.
 */

int storage_stat(const char *path, struct storage_stat *out)
{
    struct where w;
    int r = resolve(path, &w);

    if (r != STORAGE_OK) {
        return r;
    }

    memset(out, 0, sizeof(*out));

    switch (w.m->proto) {
    case STORAGE_DISK: return disk_stat(&w, out);
    case STORAGE_RAM:  return ram_stat(&w, out);
    case STORAGE_BIN:  return bin_stat(&w, out);
    default:           return STORAGE_ERR_NO_MOUNT;
    }
}

long storage_read(const char *path, uint64_t offset, void *buf, size_t bytes)
{
    struct where w;
    int r = resolve(path, &w);

    if (r != STORAGE_OK) {
        return r;
    }

    if (bytes == 0) {
        return 0;
    }

    switch (w.m->proto) {
    case STORAGE_DISK: return disk_read(&w, offset, buf, bytes);
    case STORAGE_RAM:  return ram_read(&w, offset, buf, bytes);
    case STORAGE_BIN:  return bin_read(&w, offset, buf, bytes);
    default:           return STORAGE_ERR_NO_MOUNT;
    }
}

long storage_write(const char *path, uint64_t offset, const void *buf, size_t bytes,
                   unsigned flags)
{
    struct where w;
    int r = resolve(path, &w);

    if (r != STORAGE_OK) {
        return r;
    }

    switch (w.m->proto) {
    case STORAGE_DISK:
        /* `/Home` makes nothing without DISK_CREATE, and says so itself. */
        return disk_write(&w, offset, buf, bytes, flags);
    case STORAGE_RAM:
        return ram_write(&w, offset, buf, bytes, flags);
    case STORAGE_BIN:
        return STORAGE_ERR_READ_ONLY;
    default:
        return STORAGE_ERR_NO_MOUNT;
    }
}

int storage_truncate(const char *path, uint64_t size)
{
    long r = storage_write(path, size, NULL, 0, STORAGE_CUT);

    return r < 0 ? (int)r : STORAGE_OK;
}

long storage_list(const char *path, uint64_t *cursor, char *names, size_t room)
{
    struct where w;
    int r = resolve(path, &w);

    if (r != STORAGE_OK) {
        return r;
    }

    switch (w.m->proto) {
    case STORAGE_DISK: return disk_list(&w, cursor, names, room);
    case STORAGE_RAM:  return ram_list(&w, cursor, names, room);
    case STORAGE_BIN:  return bin_list(&w, cursor, names, room);
    default:           return STORAGE_ERR_NO_MOUNT;
    }
}

int storage_mkdir(const char *path)
{
    struct where w;
    int r = resolve(path, &w);

    if (r != STORAGE_OK) {
        return r;
    }

    switch (w.m->proto) {
    case STORAGE_DISK: return disk_simple(&w, DISK_OP_MKDIR, NULL);
    case STORAGE_RAM: {
        struct storage_stat st;

        /* The ram server makes a folder over a file's name quietly. */
        if (ram_stat(&w, &st) == STORAGE_OK) {
            return STORAGE_ERR_EXISTS;
        }

        return ram_simple(&w, RAM_OP_MKDIR, NULL);
    }
    case STORAGE_BIN:  return STORAGE_ERR_READ_ONLY;
    default:           return STORAGE_ERR_NO_MOUNT;
    }
}

int storage_delete(const char *path)
{
    struct where w;
    int r = resolve(path, &w);

    if (r != STORAGE_OK) {
        return r;
    }

    switch (w.m->proto) {
    case STORAGE_DISK: return disk_simple(&w, DISK_OP_DELETE, NULL);
    case STORAGE_RAM:  return ram_simple(&w, RAM_OP_DELETE, NULL);
    case STORAGE_BIN:  return STORAGE_ERR_READ_ONLY;
    default:           return STORAGE_ERR_NO_MOUNT;
    }
}

int storage_rename(const char *from, const char *to)
{
    struct where a, b;
    int r = resolve(from, &a);

    if (r != STORAGE_OK || (r = resolve(to, &b)) != STORAGE_OK) {
        return r;
    }

    if (a.m != b.m) {
        return STORAGE_ERR_CROSSES;
    }

    switch (a.m->proto) {
    case STORAGE_DISK: return disk_simple(&a, DISK_OP_RENAME, b.path);
    case STORAGE_RAM:  return ram_simple(&a, RAM_OP_RENAME, b.path);
    case STORAGE_BIN:  return STORAGE_ERR_READ_ONLY;
    default:           return STORAGE_ERR_NO_MOUNT;
    }
}

int storage_flush(const char *path)
{
    struct where w;
    int r = resolve(path, &w);

    if (r != STORAGE_OK) {
        return r;
    }

    if (w.m->proto == STORAGE_DISK) {
        return disk_simple(&w, DISK_OP_FLUSH, NULL);
    }

    return STORAGE_OK;                  /* nothing between it and memory */
}
