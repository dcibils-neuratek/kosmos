/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A folder of `/Home`, in a sandbox** - `docs/sandboxes.md` S3.
 *
 * Each folder a sandbox is given is a virtio-fs device (VIRTIO 1.2, 5.11)
 * on the virtio-mmio transport (4.2.2): its registers are emulated here by
 * the stage-2 faults the guest takes touching them, its queues are read and
 * written in the guest's RAM directly - this process maps it - and what
 * arrives on them is FUSE (Linux's `include/uapi/linux/fuse.h`, protocol
 * 7.31), answered from the folder through the disk server's protocol
 * (`diskproto.h`).
 *
 * **The folder is the whole of it.** The monitor is handed `/Home` by the
 * namespace, as `smbd` is, and every path here is the folder's path with a
 * name the guest gave appended: a name with a `/` in it, `.` or `..` is
 * refused, so nothing above the folder can be named. A folder given read
 * only refuses every change with EROFS.
 *
 * **A write replaces a file** in `diskproto.h`, where FUSE writes at an
 * offset - so a file opened for writing is kept whole here, in memory,
 * changed in place, and written back when the guest flushes or closes it.
 * Up to `BIG` bytes; past that EFBIG.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "diskproto.h"
#include "vmmfs.h"

/* virtio-mmio's registers (VIRTIO 1.2, 4.2.2), and virtio-fs's number. */
#define MAGIC           0x000
#define VERSION         0x004
#define DEVICE_ID       0x008
#define VENDOR_ID       0x00c
#define DEV_FEATURES    0x010
#define DEV_FEAT_SEL    0x014
#define DRV_FEATURES    0x020
#define DRV_FEAT_SEL    0x024
#define QUEUE_SEL       0x030
#define QUEUE_NUM_MAX   0x034
#define QUEUE_NUM       0x038
#define QUEUE_READY     0x044
#define QUEUE_NOTIFY    0x050
#define INT_STATUS      0x060
#define INT_ACK         0x064
#define STATUS          0x070
#define DESC_LOW        0x080
#define DESC_HIGH       0x084
#define AVAIL_LOW       0x090
#define AVAIL_HIGH      0x094
#define USED_LOW        0x0a0
#define USED_HIGH       0x0a4
#define CONFIG_GEN      0x0fc
#define CONFIG          0x100

#define VIRTIO_ID_FS    26u
#define QUEUES          2u                  /* hiprio, and one request queue */
#define QUEUE_MOST      128u

#define DESC_NEXT       1u
#define DESC_WRITE      2u

/* FUSE: the operations answered, and the errors said. */
enum {
    FUSE_LOOKUP = 1, FUSE_FORGET = 2, FUSE_GETATTR = 3, FUSE_SETATTR = 4,
    FUSE_MKDIR = 9, FUSE_UNLINK = 10, FUSE_RMDIR = 11, FUSE_RENAME = 12,
    FUSE_OPEN = 14, FUSE_READ = 15, FUSE_WRITE = 16, FUSE_STATFS = 17,
    FUSE_RELEASE = 18, FUSE_FSYNC = 20, FUSE_FLUSH = 25, FUSE_INIT = 26,
    FUSE_OPENDIR = 27, FUSE_READDIR = 28, FUSE_RELEASEDIR = 29,
    FUSE_FSYNCDIR = 30, FUSE_ACCESS = 34, FUSE_CREATE = 35,
    FUSE_INTERRUPT = 36, FUSE_DESTROY = 38, FUSE_BATCH_FORGET = 42,
    FUSE_RENAME2 = 45,
};

#define E_NOENT         2
#define E_IO            5
#define E_EXIST         17
#define E_NOTDIR        20
#define E_ISDIR         21
#define E_INVAL         22
#define E_FBIG          27
#define E_ROFS          30
#define E_NAMETOOLONG   36
#define E_NOSYS         38
#define E_NOTEMPTY      39

#define O_ACCMODE       3
#define O_TRUNC         01000

#define BIG             (16u * 1024 * 1024)
#define READ_MOST       (128u * 1024)
#define REQUEST_MOST    (READ_MOST + 4096)

struct queue {
    uint32_t num;
    bool     ready;
    uint64_t desc, avail, used;
    uint16_t last;                          /* the next avail entry to take */
};

struct node {
    char    *path;                          /* "/Home/Projects/site/a.txt" */
};

struct handle {
    bool     used, dir, writing, dirty;
    uint64_t node;
    uint8_t *data;                          /* a file being written, whole */
    size_t   size;
    char    *names;                         /* a folder's names, NUL after each */
    size_t   names_bytes;
    uint32_t count;
};

struct share {
    bool     on;
    char     tag[36];
    char     root[DISK_PATH_MAX];
    bool     read_only;
    uint32_t status, int_status, dev_sel, drv_sel, sel;
    struct queue q[QUEUES];
    struct node *nodes;                     /* nodes[1] is the root */
    uint64_t nodes_n;
    struct handle *handles;
    uint64_t handles_n;
};

static struct share shares[VMMFS_MOST];
static uint8_t *ram;
static uint64_t ram_bytes, ram_base;
static long home = -1;

/*
 * A request gathered and its reply, allocated when the monitor starts:
 * every process made from the system image carries its static data, so a
 * quarter of a megabyte here would be a quarter of a megabyte in every one.
 */
static uint8_t *request;
static uint8_t *reply;

/* ---------------------------------------------------------------------
 * The guest's memory, by its physical addresses.
 * --------------------------------------------------------------------- */

static uint8_t *guest(uint64_t gpa, uint64_t len)
{
    if (gpa < ram_base || gpa - ram_base > ram_bytes || len > ram_bytes - (gpa - ram_base)) {
        return NULL;
    }

    return ram + (gpa - ram_base);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16; }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* ---------------------------------------------------------------------
 * The disk, through `/Home`: `diskproto.h` as a client, as `smbd` is.
 * --------------------------------------------------------------------- */

static struct message dmsg, drep;
static long read_cap = -1, write_cap = -1;
static uint8_t *read_at, *write_at;

static uint32_t disk_ask(uint32_t op, const char *path, uint32_t flags, uint64_t offset,
                         uint64_t bytes, long region, const char *to)
{
    struct disk_request *rq = (struct disk_request *)dmsg.data;
    const struct disk_reply *rp = (const struct disk_reply *)drep.data;

    if (home < 0) {
        return DISK_ERR_NO_DISK;
    }

    memset(&dmsg, 0, sizeof dmsg);
    memset(&drep, 0, sizeof drep);
    dmsg.length = sizeof *rq;
    dmsg.cap_plus_one = region >= 0 ? (uint32_t)region + 1 : 0;
    rq->op = op;
    rq->flags = flags;
    rq->offset = offset;
    rq->bytes = bytes;
    strncpy(rq->path, path, sizeof rq->path - 1);

    if (to != NULL) {
        strncpy(rq->u.to, to, sizeof rq->u.to - 1);
        rq->length = (uint32_t)strlen(rq->u.to);
    }

    if (kosmos_call(home, &dmsg, &drep) != 0 || drep.length < sizeof *rp) {
        return DISK_ERR_NO_DISK;
    }

    return rp->error;
}

static const struct disk_reply *disk_said(void)
{
    return (const struct disk_reply *)drep.data;
}

static bool region(long *cap, uint8_t **at, uint32_t bytes)
{
    long a;

    if (*cap >= 0) {
        return true;
    }

    if ((*cap = kosmos_mem_create(bytes / 4096u)) < 0) {
        return false;
    }

    if ((a = kosmos_mem_map(*cap)) < 0) {
        kosmos_cap_drop(*cap);
        *cap = -1;
        return false;
    }

    *at = (uint8_t *)(uintptr_t)a;
    return true;
}

/* ---------------------------------------------------------------------
 * Nodes: a number the guest names a path by, the root being 1.
 * --------------------------------------------------------------------- */

static uint64_t node_for(struct share *s, const char *path)
{
    uint64_t i;
    struct node *more;

    for (i = 1; i < s->nodes_n; i++) {
        if (s->nodes[i].path != NULL && strcmp(s->nodes[i].path, path) == 0) {
            return i;
        }
    }

    more = realloc(s->nodes, (size_t)(s->nodes_n + 1) * sizeof(*more));

    if (more == NULL) {
        return 0;
    }

    s->nodes = more;
    s->nodes[s->nodes_n].path = strdup(path);
    return s->nodes[s->nodes_n].path != NULL ? s->nodes_n++ : 0;
}

static const char *path_of(struct share *s, uint64_t node)
{
    return node >= 1 && node < s->nodes_n ? s->nodes[node].path : NULL;
}

/* A name in a folder - refused if it could name anything outside it. */
static int child(struct share *s, uint64_t parent, const char *name, char *out)
{
    const char *base = path_of(s, parent);

    if (base == NULL) {
        return -E_NOENT;
    }

    if (name[0] == '\0' || strchr(name, '/') != NULL || strcmp(name, ".") == 0
        || strcmp(name, "..") == 0) {
        return -E_INVAL;
    }

    if (strlen(base) + 1 + strlen(name) >= DISK_PATH_MAX) {
        return -E_NAMETOOLONG;
    }

    strcpy(out, base);
    strcat(out, "/");
    strcat(out, name);
    return 0;
}

/* ---------------------------------------------------------------------
 * Attributes, as FUSE says them (`struct fuse_attr`, 88 bytes).
 * --------------------------------------------------------------------- */

static int attr(struct share *s, uint64_t node, uint8_t *a)
{
    const char *path = path_of(s, node);
    const struct disk_node *n;
    bool dir;

    if (path == NULL || disk_ask(DISK_OP_GETATTR, path, 0, 0, 0, -1, NULL) != DISK_OK) {
        return -E_NOENT;
    }

    n = &disk_said()->node;

    if (n->kind != DISK_KIND_FILE && n->kind != DISK_KIND_DIR && node != 1) {
        return -E_NOENT;
    }

    dir = n->kind != DISK_KIND_FILE;
    memset(a, 0, 88);
    put64(a + 0, node);                                     /* ino */
    put64(a + 8, dir ? 0 : n->size);                        /* size */
    put64(a + 16, dir ? 0 : (n->size + 511) / 512);         /* blocks */
    put64(a + 24, n->dated ? n->modified : 0);              /* atime */
    put64(a + 32, n->dated ? n->modified : 0);              /* mtime */
    put64(a + 40, n->dated ? n->modified : 0);              /* ctime */
    put32(a + 60, (dir ? 0040000u : 0100000u)
                  | (s->read_only ? (dir ? 0555u : 0444u) : (dir ? 0755u : 0644u)));
    put32(a + 64, dir ? 2 : 1);                             /* nlink */
    put32(a + 80, 4096);                                    /* blksize */
    return 0;
}

/* ---------------------------------------------------------------------
 * Handles: an open folder's names, or a file being written.
 * --------------------------------------------------------------------- */

static uint64_t handle_new(struct share *s)
{
    uint64_t i;
    struct handle *more;

    for (i = 1; i < s->handles_n; i++) {
        if (!s->handles[i].used) {
            break;
        }
    }

    if (i >= s->handles_n || s->handles == NULL) {
        i = s->handles_n;
        more = realloc(s->handles, (size_t)(s->handles_n + 1) * sizeof(*more));

        if (more == NULL) {
            return 0;
        }

        s->handles = more;
        s->handles_n++;
    }

    memset(&s->handles[i], 0, sizeof(s->handles[i]));
    s->handles[i].used = true;
    return i;
}

static struct handle *handle(struct share *s, uint64_t fh)
{
    return fh >= 1 && fh < s->handles_n && s->handles[fh].used ? &s->handles[fh] : NULL;
}

/* A file whole into memory, for writing. */
static bool load_whole(struct share *s, struct handle *h, bool empty)
{
    const char *path = path_of(s, h->node);
    uint64_t at = 0;

    h->data = NULL;
    h->size = 0;

    if (empty) {
        return true;
    }

    while (path != NULL && region(&read_cap, &read_at, READ_MOST)) {
        uint64_t got;
        uint8_t *more;

        if (disk_ask(DISK_OP_READ, path, DISK_REGION, at, READ_MOST, read_cap, NULL) != DISK_OK) {
            return at == 0;                 /* a new file has nothing to read */
        }

        got = disk_said()->bytes;

        if (got == 0) {
            return true;
        }

        if (at + got > BIG || (more = realloc(h->data, (size_t)(at + got))) == NULL) {
            return false;
        }

        h->data = more;
        memcpy(h->data + at, read_at, (size_t)got);
        at += got;
        h->size = (size_t)at;

        if (got < READ_MOST) {
            return true;
        }
    }

    return false;
}

/* A file being written, back to the disk whole. */
static int write_back(struct share *s, struct handle *h)
{
    const char *path = path_of(s, h->node);
    uint32_t pages;

    if (!h->dirty) {
        return 0;
    }

    pages = (uint32_t)((h->size + 4095) / 4096);

    if (write_cap >= 0) {
        kosmos_cap_drop(write_cap);
        write_cap = -1;
    }

    if (!region(&write_cap, &write_at, pages > 0 ? pages * 4096 : 4096)) {
        return -E_IO;
    }

    if (h->size > 0) {
        memcpy(write_at, h->data, h->size);
    }

    if (path == NULL
        || disk_ask(DISK_OP_WRITE, path, DISK_REGION, 0, h->size, write_cap, NULL) != DISK_OK) {
        return -E_IO;
    }

    h->dirty = false;
    return 0;
}

static void handle_free(struct share *s, uint64_t fh)
{
    struct handle *h = handle(s, fh);

    if (h != NULL) {
        free(h->data);
        free(h->names);
        memset(h, 0, sizeof(*h));
    }
}

/* ---------------------------------------------------------------------
 * FUSE, one request: `in` after its 40-byte header, answered into `out`
 * after its 16-byte one. Answers the reply's body length, or -errno, or
 * NO_REPLY for the requests FUSE expects none for.
 * --------------------------------------------------------------------- */

#define NO_REPLY        (-1000000)

static long entry_reply(struct share *s, const char *path, uint8_t *out)
{
    uint64_t node = node_for(s, path);

    if (node == 0) {
        return -E_IO;
    }

    memset(out, 0, 40);
    put64(out + 0, node);                   /* nodeid */
    put64(out + 16, 1);                     /* entry_valid: a second */
    put64(out + 24, 1);                     /* attr_valid */

    return attr(s, node, out + 40) == 0 ? 128 : -E_NOENT;
}

static long fuse(struct share *s, uint32_t op, uint64_t nodeid, const uint8_t *in,
                 uint32_t in_len, uint8_t *out, uint32_t out_room)
{
    char path[DISK_PATH_MAX], path2[DISK_PATH_MAX];
    int err;

    switch (op) {
    case FUSE_INIT:
        memset(out, 0, 64);
        put32(out + 0, 7);                  /* major */
        put32(out + 4, 31);                 /* minor */
        put32(out + 8, in_len >= 12 ? get32(in + 8) : 0);   /* max_readahead */
        put16(out + 16, 16);                /* max_background */
        put16(out + 18, 12);                /* congestion_threshold */
        put32(out + 20, READ_MOST);         /* max_write */
        put32(out + 24, 1);                 /* time_gran */
        return 64;

    case FUSE_DESTROY:
        return 0;

    case FUSE_FORGET:
    case FUSE_BATCH_FORGET:
    case FUSE_INTERRUPT:
        return NO_REPLY;

    case FUSE_LOOKUP:
        if ((err = child(s, nodeid, (const char *)in, path)) != 0) {
            return err;
        }

        if (disk_ask(DISK_OP_GETATTR, path, 0, 0, 0, -1, NULL) != DISK_OK) {
            return -E_NOENT;
        }

        return entry_reply(s, path, out);

    case FUSE_GETATTR:
        memset(out, 0, 16);
        put64(out + 0, 1);                  /* attr_valid */
        return attr(s, nodeid, out + 16) == 0 ? 104 : -E_NOENT;

    case FUSE_ACCESS:
        return 0;

    case FUSE_STATFS: {
        memset(out, 0, 80);
        put64(out + 0, 1u << 20);           /* blocks */
        put64(out + 8, 1u << 19);           /* bfree */
        put64(out + 16, 1u << 19);          /* bavail */
        put64(out + 24, 1u << 16);          /* files */
        put64(out + 32, 1u << 15);          /* ffree */
        put32(out + 40, 4096);              /* bsize */
        put32(out + 44, 255);               /* namelen */
        put32(out + 48, 4096);              /* frsize */
        return 80;
    }

    case FUSE_OPENDIR: {
        const char *p = path_of(s, nodeid);
        uint64_t fh, offset = 0;
        struct handle *h;

        if (p == NULL || (fh = handle_new(s)) == 0) {
            return -E_NOENT;
        }

        h = handle(s, fh);
        h->dir = true;
        h->node = nodeid;

        for (;;) {
            const struct disk_reply *rp;
            char *bigger;

            if (disk_ask(DISK_OP_LIST, p, 0, offset, 0, -1, NULL) != DISK_OK) {
                handle_free(s, fh);
                return -E_NOTDIR;
            }

            rp = disk_said();
            bigger = realloc(h->names, h->names_bytes + rp->length + 1);

            if (bigger == NULL) {
                handle_free(s, fh);
                return -E_IO;
            }

            h->names = bigger;
            memcpy(h->names + h->names_bytes, rp->u.data, rp->length);
            h->names_bytes += rp->length;
            h->count += rp->count;

            if (!rp->more) {
                break;
            }

            offset = rp->offset;
        }

        memset(out, 0, 16);
        put64(out, fh);
        return 16;
    }

    case FUSE_READDIR: {
        struct handle *h = handle(s, get64(in));
        uint64_t from = get64(in + 8), i;
        uint32_t room = get32(in + 16), used = 0;
        const char *name;

        if (h == NULL || !h->dir) {
            return -E_INVAL;
        }

        if (room > out_room) {
            room = out_room;
        }

        /* `.`, `..`, then the folder's names - each a `fuse_dirent`, its
         * `off` the cookie the next read starts after. */
        name = h->names;

        for (i = 0; i < (uint64_t)h->count + 2; i++) {
            const char *n = i == 0 ? "." : i == 1 ? ".." : name;
            size_t len = strlen(n), rec = (24 + len + 7) & ~(size_t)7;

            if (i >= from) {
                if (used + rec > room) {
                    break;
                }

                memset(out + used, 0, rec);
                put64(out + used + 0, i < 2 ? 1 : 0xffffffff);     /* ino: unknown */
                put64(out + used + 8, i + 1);                       /* off */
                put32(out + used + 16, (uint32_t)len);
                memcpy(out + used + 24, n, len);
                used += (uint32_t)rec;
            }

            if (i >= 2) {
                name += strlen(name) + 1;
            }
        }

        return used;
    }

    case FUSE_RELEASEDIR:
    case FUSE_FSYNCDIR:
        if (op == FUSE_RELEASEDIR) {
            handle_free(s, get64(in));
        }

        return 0;

    case FUSE_OPEN: {
        uint32_t flags = get32(in);
        bool writing = (flags & O_ACCMODE) != 0;
        uint64_t fh;
        struct handle *h;
        uint8_t a[88];

        if (writing && s->read_only) {
            return -E_ROFS;
        }

        if (attr(s, nodeid, a) != 0) {
            return -E_NOENT;
        }

        if ((get32(a + 60) & 0170000u) == 0040000u) {
            return -E_ISDIR;
        }

        if ((fh = handle_new(s)) == 0) {
            return -E_IO;
        }

        h = handle(s, fh);
        h->node = nodeid;
        h->writing = writing;

        if (writing && !load_whole(s, h, (flags & O_TRUNC) != 0)) {
            handle_free(s, fh);
            return -E_FBIG;
        }

        h->dirty = writing && (flags & O_TRUNC) != 0;
        memset(out, 0, 16);
        put64(out, fh);
        return 16;
    }

    case FUSE_READ: {
        struct handle *h = handle(s, get64(in));
        uint64_t offset = get64(in + 8);
        uint32_t size = get32(in + 16);
        const char *p = path_of(s, nodeid);

        if (size > READ_MOST) {
            size = READ_MOST;
        }

        if (size > out_room) {
            size = out_room;
        }

        if (h != NULL && h->writing) {
            uint64_t n = offset >= h->size ? 0 : h->size - offset;

            n = n < size ? n : size;
            memcpy(out, h->data + offset, (size_t)n);
            return (long)n;
        }

        if (p == NULL || !region(&read_cap, &read_at, READ_MOST)) {
            return -E_IO;
        }

        if (disk_ask(DISK_OP_READ, p, DISK_REGION, offset, size, read_cap, NULL) != DISK_OK) {
            return 0;                       /* past its end */
        }

        memcpy(out, read_at, (size_t)disk_said()->bytes);
        return (long)disk_said()->bytes;
    }

    case FUSE_WRITE: {
        struct handle *h = handle(s, get64(in));
        uint64_t offset = get64(in + 8);
        uint32_t size = get32(in + 16);
        uint8_t *bigger;

        if (h == NULL || !h->writing) {
            return s->read_only ? -E_ROFS : -E_INVAL;
        }

        if (in_len < 40 + size || offset + size > BIG) {
            return in_len < 40 + size ? -E_INVAL : -E_FBIG;
        }

        if (offset + size > h->size) {
            if ((bigger = realloc(h->data, (size_t)(offset + size))) == NULL) {
                return -E_IO;
            }

            memset(bigger + h->size, 0, (size_t)(offset + size - h->size));
            h->data = bigger;
            h->size = (size_t)(offset + size);
        }

        memcpy(h->data + offset, in + 40, size);
        h->dirty = true;
        memset(out, 0, 8);
        put32(out, size);
        return 8;
    }

    case FUSE_FLUSH:
    case FUSE_FSYNC: {
        struct handle *h = handle(s, get64(in));

        return h != NULL ? write_back(s, h) : 0;
    }

    case FUSE_RELEASE: {
        struct handle *h = handle(s, get64(in));
        int said = h != NULL ? write_back(s, h) : 0;

        handle_free(s, get64(in));
        return said;
    }

    case FUSE_CREATE: {
        uint32_t flags = get32(in);
        uint64_t fh;
        struct handle *h;
        long body;

        if (s->read_only) {
            return -E_ROFS;
        }

        if ((err = child(s, nodeid, (const char *)in + 16, path)) != 0) {
            return err;
        }

        if (disk_ask(DISK_OP_WRITE, path, 0, 0, 0, -1, NULL) != DISK_OK) {
            return -E_IO;
        }

        if ((body = entry_reply(s, path, out)) < 0 || (fh = handle_new(s)) == 0) {
            return body < 0 ? body : -E_IO;
        }

        h = handle(s, fh);
        h->node = get64(out);
        h->writing = true;                  /* made to be written */
        (void)flags;
        memset(out + 128, 0, 16);
        put64(out + 128, fh);
        return 144;
    }

    case FUSE_MKDIR:
        if (s->read_only) {
            return -E_ROFS;
        }

        if ((err = child(s, nodeid, (const char *)in + 8, path)) != 0) {
            return err;
        }

        if (disk_ask(DISK_OP_MKDIR, path, 0, 0, 0, -1, NULL) != DISK_OK) {
            return -E_EXIST;
        }

        return entry_reply(s, path, out);

    case FUSE_UNLINK:
    case FUSE_RMDIR:
        if (s->read_only) {
            return -E_ROFS;
        }

        if ((err = child(s, nodeid, (const char *)in, path)) != 0) {
            return err;
        }

        return disk_ask(DISK_OP_DELETE, path, 0, 0, 0, -1, NULL) == DISK_OK
               ? 0 : (op == FUSE_RMDIR ? -E_NOTEMPTY : -E_NOENT);

    case FUSE_RENAME:
    case FUSE_RENAME2: {
        size_t head = op == FUSE_RENAME ? 8 : 16;
        const char *from = (const char *)in + head;
        const char *to = from + strlen(from) + 1;

        if (s->read_only) {
            return -E_ROFS;
        }

        if ((err = child(s, nodeid, from, path)) != 0
            || (err = child(s, get64(in), to, path2)) != 0) {
            return err;
        }

        return disk_ask(DISK_OP_RENAME, path, 0, 0, 0, -1, path2) == DISK_OK ? 0 : -E_IO;
    }

    case FUSE_SETATTR: {
        uint32_t valid = get32(in);

        /* A size: truncation, of a file open for writing or not. */
        if ((valid & (1u << 3)) != 0) {
            struct handle t, *h = (valid & (1u << 6)) != 0 ? handle(s, get64(in + 8)) : NULL;
            uint64_t size = get64(in + 16);

            if (s->read_only) {
                return -E_ROFS;
            }

            if (h == NULL) {
                memset(&t, 0, sizeof(t));
                t.node = nodeid;
                h = &t;

                if (!load_whole(s, h, size == 0)) {
                    return -E_FBIG;
                }
            }

            if (size > BIG) {
                return -E_FBIG;
            }

            if (size != h->size) {
                uint8_t *bigger = realloc(h->data, (size_t)(size > 0 ? size : 1));

                if (bigger == NULL) {
                    return -E_IO;
                }

                if (size > h->size) {
                    memset(bigger + h->size, 0, (size_t)(size - h->size));
                }

                h->data = bigger;
                h->size = (size_t)size;
            }

            h->dirty = true;

            if (h == &t) {
                err = write_back(s, h);
                free(t.data);

                if (err != 0) {
                    return err;
                }
            }
        }

        memset(out, 0, 16);
        put64(out, 1);
        return attr(s, nodeid, out + 16) == 0 ? 104 : -E_NOENT;
    }

    default:
        return -E_NOSYS;
    }
}

/* ---------------------------------------------------------------------
 * A queue: every request the guest has made available, answered.
 * --------------------------------------------------------------------- */

static void serve_queue(struct share *s, struct queue *q)
{
    uint8_t *avail = guest(q->avail, 4 + 2 * (uint64_t)q->num);
    uint8_t *used = guest(q->used, 4 + 8 * (uint64_t)q->num);
    uint8_t *desc = guest(q->desc, 16 * (uint64_t)q->num);

    if (!q->ready || avail == NULL || used == NULL || desc == NULL || q->num == 0
        || request == NULL || reply == NULL) {
        return;
    }

    while (q->last != get16(avail + 2)) {
        uint16_t head = get16(avail + 4 + 2 * (q->last % q->num));
        uint16_t d = head;
        uint32_t in_len = 0, written = 0, hops = 0;
        uint8_t *outs[QUEUE_MOST];
        uint32_t out_lens[QUEUE_MOST], nout = 0;
        long body;

        /* Gather what the guest wrote; note where the answer goes. */
        for (;;) {
            uint8_t *e = desc + 16 * (uint64_t)(d % q->num);
            uint64_t addr = get64(e);
            uint32_t len = get32(e + 8);
            uint16_t flags = get16(e + 12);
            uint8_t *at = guest(addr, len);

            if (at != NULL) {
                if ((flags & DESC_WRITE) != 0) {
                    if (nout < QUEUE_MOST) {
                        outs[nout] = at;
                        out_lens[nout++] = len;
                    }
                } else if (in_len + len <= REQUEST_MOST) {
                    memcpy(request + in_len, at, len);
                    in_len += len;
                }
            }

            if ((flags & DESC_NEXT) == 0 || ++hops >= q->num) {
                break;
            }

            d = get16(e + 14);
        }

        /* Answer: `fuse_out_header` and the body, scattered back. */
        if (in_len >= 40) {
            uint32_t op = get32(request + 4);
            uint64_t unique = get64(request + 8), nodeid = get64(request + 16);
            uint32_t room = 0, i;

            for (i = 0; i < nout; i++) {
                room += out_lens[i];
            }

            room = room > REQUEST_MOST ? REQUEST_MOST : room;
            body = fuse(s, op, nodeid, request + 40, in_len - 40, reply + 16,
                        room > 16 ? room - 16 : 0);

            if (body != NO_REPLY && room >= 16) {
                uint32_t total = 16 + (body > 0 ? (uint32_t)body : 0), left;

                put32(reply, total);
                put32(reply + 4, body < 0 ? (uint32_t)body : 0);
                put64(reply + 8, unique);
                left = total;

                for (i = 0; i < nout && left > 0; i++) {
                    uint32_t n = left < out_lens[i] ? left : out_lens[i];

                    memcpy(outs[i], reply + (total - left), n);
                    left -= n;
                }

                written = total - left;
            }
        }

        put32(used + 4 + 8 * (get16(used + 2) % q->num), head);
        put32(used + 4 + 8 * (get16(used + 2) % q->num) + 4, written);
        put16(used + 2, (uint16_t)(get16(used + 2) + 1));
        q->last++;
        s->int_status |= 1;
    }
}

/* ---------------------------------------------------------------------
 * The transport's registers.
 * --------------------------------------------------------------------- */

static void reset(struct share *s)
{
    s->status = 0;
    s->int_status = 0;
    s->dev_sel = s->drv_sel = s->sel = 0;
    memset(s->q, 0, sizeof(s->q));
}

bool vmmfs_mmio(unsigned i, uint64_t off, unsigned size, bool write, uint64_t *value)
{
    struct share *s = i < VMMFS_MOST ? &shares[i] : NULL;
    struct queue *q;

    if (s == NULL || !s->on) {
        return false;
    }

    q = &s->q[s->sel < QUEUES ? s->sel : 0];

    if (off >= CONFIG) {
        /* `struct virtio_fs_config`: its tag, and one request queue. */
        uint8_t cfg[40];
        uint64_t at = off - CONFIG, v = 0;
        unsigned k;

        memset(cfg, 0, sizeof(cfg));
        memcpy(cfg, s->tag, strlen(s->tag));
        put32(cfg + 36, 1);

        for (k = 0; !write && k < size && at + k < sizeof(cfg); k++) {
            v |= (uint64_t)cfg[at + k] << (8 * k);
        }

        if (!write) {
            *value = v;
        }

        return true;
    }

    if (write) {
        uint32_t v = (uint32_t)*value;

        switch (off) {
        case DEV_FEAT_SEL: s->dev_sel = v; break;
        case DRV_FEAT_SEL: s->drv_sel = v; break;
        case QUEUE_SEL:    s->sel = v; break;
        case QUEUE_NUM:    q->num = v <= QUEUE_MOST ? v : QUEUE_MOST; break;
        case QUEUE_READY:  q->ready = v != 0; break;
        case QUEUE_NOTIFY:
            if (v < QUEUES) {
                serve_queue(s, &s->q[v]);
            }
            break;
        case INT_ACK:      s->int_status &= ~v; break;
        case STATUS:
            if (v == 0) {
                reset(s);
            } else {
                s->status = v;
            }
            break;
        case DESC_LOW:     q->desc = (q->desc & ~0xffffffffull) | v; break;
        case DESC_HIGH:    q->desc = (q->desc & 0xffffffffull) | (uint64_t)v << 32; break;
        case AVAIL_LOW:    q->avail = (q->avail & ~0xffffffffull) | v; break;
        case AVAIL_HIGH:   q->avail = (q->avail & 0xffffffffull) | (uint64_t)v << 32; break;
        case USED_LOW:     q->used = (q->used & ~0xffffffffull) | v; break;
        case USED_HIGH:    q->used = (q->used & 0xffffffffull) | (uint64_t)v << 32; break;
        default: break;                     /* driver features: VERSION_1 is all */
        }

        return true;
    }

    switch (off) {
    case MAGIC:         *value = 0x74726976; break;     /* "virt" */
    case VERSION:       *value = 2; break;
    case DEVICE_ID:     *value = VIRTIO_ID_FS; break;
    case VENDOR_ID:     *value = 0x534f4d4b; break;     /* "KMOS" */
    case DEV_FEATURES:  *value = s->dev_sel == 1 ? 1 : 0; break;   /* VIRTIO_F_VERSION_1 */
    case QUEUE_NUM_MAX: *value = s->sel < QUEUES ? QUEUE_MOST : 0; break;
    case QUEUE_READY:   *value = q->ready ? 1 : 0; break;
    case INT_STATUS:    *value = s->int_status; break;
    case STATUS:        *value = s->status; break;
    case CONFIG_GEN:    *value = 0; break;
    default:            *value = 0; break;
    }

    return true;
}

bool vmmfs_raised(unsigned i)
{
    return i < VMMFS_MOST && shares[i].on && shares[i].int_status != 0;
}

bool vmmfs_add(unsigned i, const char *tag, const char *root, bool read_only)
{
    struct share *s = i < VMMFS_MOST ? &shares[i] : NULL;

    if (s == NULL || strlen(tag) >= sizeof(s->tag) || strlen(root) >= sizeof(s->root)
        || strncmp(root, "/Home/", 6) != 0 || strstr(root, "/..") != NULL) {
        return false;
    }

    memset(s, 0, sizeof(*s));
    strcpy(s->tag, tag);
    strcpy(s->root, root);
    s->read_only = read_only;
    s->nodes_n = 1;                         /* 0 is no node */
    s->handles_n = 1;                       /* and 0 is no handle */

    if (node_for(s, root) != 1) {
        return false;
    }

    s->on = true;
    return true;
}

void vmmfs_start(long home_cap, uint8_t *guest_ram, uint64_t guest_ram_bytes, uint64_t base)
{
    home = home_cap;
    request = malloc(REQUEST_MOST);
    reply = malloc(REQUEST_MOST);
    ram = guest_ram;
    ram_bytes = guest_ram_bytes;
    ram_base = base;
}
