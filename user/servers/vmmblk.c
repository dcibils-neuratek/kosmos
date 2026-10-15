/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A sandbox's disk: virtio-blk, backed by a file (`vmmblk.h`).
 *
 * A request is a chain: a 16-byte header the guest wrote - its type, and
 * the sector it starts at - then, for a write, the bytes; for a read, room
 * for them; and last a byte for the status, which this writes. Bytes cross
 * between the guest's RAM and the file through one region, a piece at a
 * time, so no request of any size is a message's payload
 * (`CLAUDE.md`, control by message, data by shared memory).
 *
 * **The vCPU waits for its disk**, since this runs on the thread the guest
 * does: a request is answered before the guest goes on. One processor and
 * a stick do not need more; the queue can move to a thread of its own when
 * a measurement says the guest is waiting on it.
 */

#include <stddef.h>
#include <string.h>

#include "diskask.h"
#include "vdev.h"
#include "vmmblk.h"

#define VIRTIO_ID_BLOCK     2u
#define BLK_F_SEG_MAX       (1ull << 2)
#define BLK_F_RO            (1ull << 5)
#define BLK_F_BLK_SIZE      (1ull << 6)
#define BLK_F_FLUSH         (1ull << 9)

#define T_IN                0u
#define T_OUT               1u
#define T_FLUSH             4u
#define T_GET_ID            8u

#define S_OK                0u
#define S_IOERR             1u
#define S_UNSUPP            2u

#define SECTOR              512u
#define PIECE               (1024u * 1024u)     /* the region: a read or write a call */

static struct {
    bool            on, read_only;
    struct vdev     dev;
    struct diskask  disk;
    char            path[DISK_PATH_MAX];
    uint64_t        bytes;
    long            region;
    uint8_t        *at;
    uint64_t        reads, writes;
} B = { .region = -1, .disk = { .ep = -1 } };

/* `n` bytes at `pos` of a chain's buffers of one side, copied to or from
 * `p`: how many there were. */
static uint32_t across(uint8_t *const *seg, const uint32_t *len, unsigned nseg,
                       uint64_t pos, uint8_t *p, uint32_t n, bool into_chain)
{
    uint32_t done = 0;

    for (unsigned i = 0; i < nseg && done < n; i++) {
        if (pos >= len[i]) {
            pos -= len[i];
            continue;
        }

        uint32_t take = len[i] - (uint32_t)pos;

        if (take > n - done) {
            take = n - done;
        }

        if (into_chain) {
            memcpy(seg[i] + pos, p + done, take);
        } else {
            memcpy(p + done, seg[i] + pos, take);
        }

        done += take;
        pos = 0;
    }

    return done;
}

static uint64_t total(const uint32_t *len, unsigned nseg)
{
    uint64_t t = 0;

    for (unsigned i = 0; i < nseg; i++) {
        t += len[i];
    }

    return t;
}

/* Bytes of the file into the guest's buffers, a piece at a time. */
static uint8_t read_into(struct vdev_chain *c, uint64_t offset, uint64_t n)
{
    uint64_t done = 0;

    while (done < n) {
        uint32_t piece = n - done < PIECE ? (uint32_t)(n - done) : PIECE;

        if (diskask(&B.disk, DISK_OP_READ, B.path, DISK_REGION, offset + done, piece,
                    B.region, NULL, 0) != DISK_OK
            || diskask_said(&B.disk)->bytes != piece) {
            return S_IOERR;
        }

        across(c->out, c->out_len, c->nout, done, B.at, piece, true);
        done += piece;
    }

    B.reads += n;
    return S_OK;
}

/* The guest's bytes, after its header, onto the file where they belong. */
static uint8_t write_from(struct vdev_chain *c, uint64_t offset, uint64_t n)
{
    uint64_t done = 0;

    while (done < n) {
        uint32_t piece = n - done < PIECE ? (uint32_t)(n - done) : PIECE;

        across(c->in, c->in_len, c->nin, 16 + done, B.at, piece, false);

        if (diskask(&B.disk, DISK_OP_WRITE_AT, B.path, DISK_REGION | DISK_IN_PLACE,
                    offset + done, piece, B.region, NULL, 0) != DISK_OK) {
            return S_IOERR;
        }

        done += piece;
    }

    B.writes += n;
    return S_OK;
}

static void notify(struct vdev *d, unsigned q)
{
    struct vdev_chain c;

    while (vdev_pop(d, q, &c)) {
        uint8_t head[16], status = S_UNSUPP;
        uint64_t room = total(c.out_len, c.nout), written = 1;
        uint32_t type;
        uint64_t sector, offset, n;

        if (room == 0 || across(c.in, c.in_len, c.nin, 0, head, 16, false) != 16) {
            vdev_push(d, q, c.head, 0);
            continue;
        }

        memcpy(&type, head, 4);
        memcpy(&sector, head + 8, 8);
        offset = sector * SECTOR;

        switch (type) {
        case T_IN:
            n = room - 1;

            if (sector > B.bytes / SECTOR || n > B.bytes - offset || n % SECTOR != 0) {
                status = S_IOERR;
            } else {
                status = read_into(&c, offset, n);
                written = status == S_OK ? n + 1 : 1;
            }
            break;
        case T_OUT:
            n = total(c.in_len, c.nin) - 16;

            if (B.read_only || sector > B.bytes / SECTOR || n > B.bytes - offset
                || n % SECTOR != 0) {
                status = S_IOERR;
            } else {
                status = write_from(&c, offset, n);
            }
            break;
        case T_FLUSH:
            status = diskask(&B.disk, DISK_OP_FLUSH, B.path, 0, 0, 0, -1, NULL, 0) == DISK_OK
                     ? S_OK : S_IOERR;
            break;
        case T_GET_ID: {
            char id[20] = "kosmos-disk";

            n = room - 1 < sizeof id ? room - 1 : sizeof id;
            across(c.out, c.out_len, c.nout, 0, (uint8_t *)id, (uint32_t)n, true);
            written = n + 1;
            status = S_OK;
            break;
        }
        default:
            break;
        }

        across(c.out, c.out_len, c.nout, room - 1, &status, 1, true);
        vdev_push(d, q, c.head, (uint32_t)written);
    }
}

/*
 * `struct virtio_blk_config`: its capacity in sectors; how many segments a
 * request may have; and a block of 4 KB, the filesystem's own.
 *
 * **Without `seg_max` a guest sends one segment a request** - a page - and
 * Linux wrote to its disk on the M700 at 1.3 MB/s, a round trip to the
 * stick each 4 KB, with its processor stuck long enough to call it a soft
 * lockup (15 October 2026). A request may carry as many as a chain holds,
 * less its header and its status.
 */
static void config(struct vdev *d, uint64_t off, uint8_t *out, unsigned size)
{
    uint8_t cfg[24];
    uint64_t sectors = B.bytes / SECTOR;
    uint32_t seg_max = VDEV_SEGMENTS - 2, blk_size = 4096;

    (void)d;
    memset(cfg, 0, sizeof cfg);
    memcpy(cfg, &sectors, 8);
    memcpy(cfg + 12, &seg_max, 4);
    memcpy(cfg + 20, &blk_size, 4);

    for (unsigned k = 0; k < size; k++) {
        out[k] = off + k < sizeof cfg ? cfg[off + k] : 0;
    }
}

bool vmmblk_start(long disk_cap, const char *path, bool read_only)
{
    const struct disk_reply *rp;

    if (disk_cap < 0 || path == NULL || strlen(path) >= sizeof B.path
        || strstr(path, "/..") != NULL) {
        return false;
    }

    B.disk.ep = disk_cap;
    strcpy(B.path, path);

    if (diskask(&B.disk, DISK_OP_GETATTR, B.path, 0, 0, 0, -1, NULL, 0) != DISK_OK) {
        return false;
    }

    rp = diskask_said(&B.disk);

    if (rp->node.kind != DISK_KIND_FILE || rp->node.size < SECTOR
        || !diskask_region(&B.region, &B.at, PIECE)) {
        return false;
    }

    B.bytes = rp->node.size / SECTOR * SECTOR;
    B.read_only = read_only;
    B.dev.id = VIRTIO_ID_BLOCK;
    B.dev.features = BLK_F_FLUSH | BLK_F_SEG_MAX | BLK_F_BLK_SIZE | (read_only ? BLK_F_RO : 0);
    B.dev.queues = 1;
    B.dev.config = config;
    B.dev.notify = notify;
    B.on = true;
    return true;
}

bool vmmblk_on(void)
{
    return B.on;
}

bool vmmblk_mmio(uint64_t off, unsigned size, bool write, uint64_t *value)
{
    return B.on && vdev_mmio(&B.dev, off, size, write, value);
}

bool vmmblk_raised(void)
{
    return B.on && vdev_raised(&B.dev);
}

void vmmblk_counted(uint64_t *reads, uint64_t *writes)
{
    *reads = B.reads;
    *writes = B.writes;
}
