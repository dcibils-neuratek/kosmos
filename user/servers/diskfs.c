/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /Home: the disk server, in C (`docs/diskfs.md` step 3).
 *
 * What the Lua disk server's `diskfs_handlers` answered, the same way, on
 * `kfs.c` and speaking `diskproto.h`: a directory's names a page at a time, a file's
 * bytes into the caller's region or a page in the reply, a file written
 * from a region or from the request, removals, renames, folders,
 * attributes, queries, and the disk itself - `.super`, `.device` and
 * `.format`. Underneath, the same things: the kernel's disk or a stick's
 * Kosmos partition behind the USB driver, the device's cost counted, the
 * small reads kept (`diskcache.c`), and a write dated from `/Devices/clock`.
 *
 * **Two things differ, both on purpose** (`docs/diskfs.md`):
 *
 *   - **A query scans what it is asked about** - the folder named and what
 *     is under it - rather than an index of the whole disk built on the
 *     first query. With no index keyed by path, no request has to be put in
 *     the disk's spelling first, which was a second walk of every path.
 *   - **What it says reaches the log.** It is handed the console's endpoint,
 *     so a replayed journal and a blank disk formatted are said, where the
 *     Lua server had no console and its `print` went nowhere.
 *
 * **Everything it works in is mapped once, when it starts** - the
 * filesystem's two megabytes, the cache, the buffers - rather than static:
 * `init.elf` is one image every process runs, and a static array here would
 * be carried by all of them.
 *
 * **Where a comment here says the Lua server has the reasoning**, it is
 * `init.lua` as it was when `docs/diskfs.md` step 4 removed that server -
 * `git show ef8617ba^:user/init/init.lua` - whose comments are the
 * long-form argument for each of these parts, as `kfs.h` says of `kfs.lua`.
 * They are not repeated here.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bytes.h"
#include "kosmos.h"
#include "blockproto.h"
#include "devproto.h"
#include "diskproto.h"

#include "../init/say.h"
#include "diskcache.h"
#include "drives_decode.h"
#include "drivers/usb/storage_decode.h"
#include "kfs.h"
#include "packflat.h"

void diskfs_server(long endpoint, long blocks_read, long blocks_write,
                   long devices, long console);

/* The names a Kosmos disk keeps for itself (the Lua server's `RESERVED`). */
static const char *const RESERVED[] = { ".super", ".format", ".device" };

/* The type a Kosmos partition has in a stick's GPT (`tools/mkusb_image.py`). */
static const char KOSMOS_PARTITION[] = "8A9DC8A8-83CF-4F7F-962B-43157A68F14A";

#define SECTOR          512u
#define STICK_MOST      BLOCK_TRANSFER_MOST
#define WAIT_SECONDS    20u

/* A listing sorts at most this many names; a query holds this many folders,
 * and keeps this much of its answer. */
#define NAMES_MAX       65536u
#define PENDING_BYTES   (64u * 1024u)
#define FOUND_BYTES     (256u * 1024u)

/*
 * Everything that is not a handful of words, mapped at the start.
 */
struct arena {
    struct kfs kfs;
    struct diskcache cache;
    struct packflat stored;
    struct packflat terms;
    uint8_t attrs[KFS_BLOCK];
    uint8_t packed[KFS_BLOCK];
    uint32_t order[NAMES_MAX];
    uint8_t pending[PENDING_BYTES];
    char path[DISK_PATH_MAX * 2];

    /*
     * **A query's whole answer, from one scan**, and what it answered: the
     * folder and the terms, and the disk as it was then. A page after the
     * first is taken from here while nothing has changed - each page
     * scanned the tree again, and an answer of two hundred files was five
     * scans (`testing.md` 18.283).
     */
    uint64_t found_generation;
    uint32_t found_key_len;
    uint32_t found_len, found_count;
    bool found_valid;
    uint8_t found_key[DISK_PATH_MAX + DISK_DATA_MAX];
    char found[FOUND_BYTES];
};

static struct arena *A;

static long console = -1;
static long devices = -1;
static long blocks_read = -1;
static long blocks_write = -1;

static struct kfs_super SB;
static bool mounted_ok;
static bool told_unreadable;

/* Every request that may have changed the disk, counted. */
static uint64_t generation;

/*
 * ------------------------------------------------------------------------
 * Saying things, to the log.
 * ------------------------------------------------------------------------
 */

static void tell(const char *a, unsigned long n, bool with_n, const char *b)
{
    struct say_line line;

    if (console < 0) {
        return;
    }

    say_begin(&line);
    say_text(&line, "disk: ");
    say_text(&line, a);

    if (with_n) {
        say_dec(&line, n);
    }

    if (b != NULL) {
        say_text(&line, b);
    }

    say_send(console, &line);
}

/* A string into a fixed field, cut and ended. */
static void put_text(char *field, size_t size, const char *text)
{
    size_t n = strlen(text);

    if (n >= size) {
        n = size - 1;
    }

    memcpy(field, text, n);
    field[n] = '\0';
}

/*
 * ------------------------------------------------------------------------
 * The date a write is stamped with (the Lua server's `stamp`, `roadmap.md` 6za
 * step b): `/Devices/clock`'s epoch and how many writes this second has
 * seen, or the counter when there is no clock - the order kept, and no
 * date claimed.
 * ------------------------------------------------------------------------
 */

static uint64_t last_second;
static uint32_t nth;

static uint64_t clock_epoch(void)
{
    struct message msg, rep;
    struct dev_request *req = (struct dev_request *)msg.data;
    const struct dev_reply *r = (const struct dev_reply *)rep.data;

    if (devices < 0) {
        return 0;
    }

    memset(&msg, 0, sizeof msg);
    msg.length = sizeof *req;
    req->op = DEV_OP_READ;
    put_text(req->name, sizeof req->name, "clock");

    if (kosmos_call(devices, &msg, &rep) != 0 || rep.length < sizeof *r
        || r->error != DEV_OK) {
        return 0;
    }

    for (uint32_t i = 0; i < r->count && i < DEV_FIELDS; i++) {
        if (strncmp(r->field[i].name, "epoch", DEV_NAME_MAX) == 0
            && r->field[i].kind == DEV_KIND_NUMBER) {
            return r->field[i].number;
        }
    }

    return 0;
}

static uint64_t stamp(void)
{
    uint64_t epoch = clock_epoch();

    if (epoch == 0) {
        return kosmos_ticks();
    }

    if (epoch == last_second) {
        nth++;
    } else {
        last_second = epoch;
        nth = 0;
    }

    return kfs_stamp(epoch, nth);
}

/*
 * ------------------------------------------------------------------------
 * The device: the kernel's disk, or a stick's Kosmos partition behind the
 * USB driver (the Lua server's `stick_home`), with what each costs counted.
 * ------------------------------------------------------------------------
 */

static struct disk_device cost;

static struct {
    bool use;                   /* /Home is on a stick */
    bool have_wanted;
    char wanted[40];            /* one partition, by its own GUID */
    long region;
    uint8_t *at;
    uint32_t handle;
    bool found;
    uint32_t unit;
    uint64_t first, sectors;
    char where[128];
    char flush_why[96];
    uint32_t looks;
    uint32_t stops[DISK_STICK_STEPS];
    uint64_t first_look, found_at;
    bool waited;
} stick;

static struct diskinfo kernel_disk;

/* One request to the USB driver, and whether it was done. */
static bool ask(long endpoint, uint32_t op, uint32_t unit, uint64_t lba,
                uint32_t count, long pass, struct block_reply *out,
                uint32_t *error)
{
    struct message msg, rep;
    struct block_request *req = (struct block_request *)msg.data;

    *error = BLOCK_ERR_DEVICE;

    if (endpoint < 0) {
        return false;
    }

    memset(&msg, 0, sizeof msg);
    msg.length = sizeof *req;
    msg.cap_plus_one = (pass >= 0) ? (uint32_t)(pass + 1) : 0u;
    req->op = op;
    req->unit = unit;
    req->lba = lba;
    req->count = count;
    req->handle = stick.handle;

    if (kosmos_call(endpoint, &msg, &rep) != 0 || rep.length < sizeof *out) {
        return false;
    }

    memcpy(out, rep.data, sizeof *out);
    *error = out->error;
    return out->error == BLOCK_OK;
}

/*
 * The first Kosmos partition on a ready stick; how far each look got.
 *
 * The stick's GPT is read by the decoders the drive server reads every
 * drive's with - `gpt_header_at` holds the header to its CRC and its place,
 * `gpt_entry_array` says where the entries are and refuses an array too
 * large to read at once, and `gpt_partitions` names each used entry with
 * both its GUIDs - rather than by a third reading of the same bytes here.
 */
static bool look(void)
{
    struct block_reply r;
    uint32_t error, stop = 0, units;

    memset(&r, 0, sizeof r);

    if (stick.found) {
        return true;
    }

    stick.looks++;

    if (stick.first_look == 0) {
        stick.first_look = kosmos_ticks();
    }

#define REACHED(n) do { if ((n) > stop) stop = (n); } while (0)

    if (stick.at == NULL) {
        long cap = kosmos_mem_create(STICK_MOST / 4096u);
        long at;

        if (cap < 0) {
            REACHED(1);
            goto stopped;
        }

        if (!ask(blocks_read, BLOCK_OP_OPEN, 0, 0, 0, cap, &r, &error)) {
            kosmos_cap_drop(cap);
            REACHED(2);
            goto stopped;
        }

        at = kosmos_mem_map(cap);

        if (at < 0) {
            REACHED(2);
            goto stopped;
        }

        stick.region = cap;
        stick.handle = r.handle;
        stick.at = (uint8_t *)(uintptr_t)at;
    }

    REACHED(3);

    /* How many units have been named: an INFO answers it whether or not the
     * unit it asked about is there (`blockproto.h`). */
    memset(&r, 0, sizeof r);
    ask(blocks_read, BLOCK_OP_INFO, 0, 0, 0, -1, &r, &error);
    units = r.count;

    for (uint32_t u = 0; u < units; u++) {
        struct block_reply info;
        struct drives_part parts[DRIVES_PARTS_MAX];
        uint64_t entries_at = 0;
        unsigned count = 0, size = 0, found;
        uint32_t need;

        if (!ask(blocks_read, BLOCK_OP_INFO, u, 0, 0, -1, &info, &error)) {
            REACHED(4);
            continue;
        }

        if (info.block_size != SECTOR) {
            REACHED(5);
            continue;
        }

        if (!ask(blocks_read, BLOCK_OP_READ, u, 1, 1, -1, &r, &error)) {
            REACHED(6);
            continue;
        }

        if (!gpt_header_at(stick.at, SECTOR, 1u)
            || !gpt_entry_array(stick.at, SECTOR, STICK_MOST, &entries_at,
                                &size, &count)) {
            REACHED(7);
            continue;
        }

        need = (uint32_t)(((uint64_t)count * size + SECTOR - 1) / SECTOR);

        if (!ask(blocks_read, BLOCK_OP_READ, u, entries_at, need, -1, &r, &error)) {
            REACHED(8);
            continue;
        }

        REACHED(9);

        /* Used entries only, each ending where it starts or after. */
        found = gpt_partitions(stick.at, need * SECTOR, size, count, parts,
                               DRIVES_PARTS_MAX);

        for (unsigned i = 0; i < found; i++) {
            char type[37], own[37];
            uint64_t lo = parts[i].first;
            uint64_t hi = parts[i].first + parts[i].sectors - 1u;

            drives_guid_text(parts[i].type_guid, type);

            if (strcmp(type, KOSMOS_PARTITION) != 0) {
                continue;
            }

            drives_guid_text(parts[i].guid, own);

            if (stick.have_wanted && strcmp(own, stick.wanted) != 0) {
                REACHED(10);
            } else if (hi < info.blocks) {
                struct say_line line;

                stick.unit = u;
                stick.first = lo;
                stick.sectors = hi - lo + 1;
                stick.found = true;
                stick.found_at = kosmos_ticks();

                say_begin(&line);
                say_text(&line, "the Kosmos partition on USB unit ");
                say_dec(&line, u);
                say_text(&line, ", blocks ");
                say_dec(&line, (unsigned long)lo);
                say_text(&line, " to ");
                say_dec(&line, (unsigned long)hi);
                put_text(stick.where, sizeof stick.where, line.text);
                return true;
            } else {
                REACHED(11);
            }
        }
    }

stopped:
    if (stop > 0) {
        stick.stops[stop - 1]++;
    }

#undef REACHED
    return false;
}

/*
 * Looked for on each request until it is found, and waited for once: nothing
 * makes init wait for the USB driver, and naming a stick takes seconds on the
 * ThinkPad (the Lua server's `stick_home` has the whole account).
 */
static bool stick_found(void)
{
    struct schedinfo s;
    unsigned long tenth = 10;

    if (look()) {
        return true;
    }

    if (stick.waited) {
        return false;
    }

    stick.waited = true;

    if (kosmos_sched_info(&s) == 0 && s.tick_hz >= 10) {
        tenth = s.tick_hz / 10;
    }

    for (unsigned i = 0; i < WAIT_SECONDS * 10; i++) {
        kosmos_sleep(tenth);

        if (look()) {
            return true;
        }
    }

    return false;
}

/* The disk there is: its sectors and the most one call moves; or why none. */
static bool device(uint64_t *sectors, uint32_t *most, const char **why)
{
    if (stick.use) {
        if (!stick_found()) {
            *why = "the USB stick's Kosmos partition is not there yet";
            return false;
        }

        *sectors = stick.sectors;
        *most = STICK_MOST / KFS_BLOCK;
        return true;
    }

    if (kernel_disk.sectors == 0 && kosmos_disk_info(&kernel_disk) != 0) {
        kernel_disk.sectors = 0;
        *why = "this machine has no disk";
        return false;
    }

    if (kernel_disk.sectors == 0) {
        *why = "this machine has no disk";
        return false;
    }

    *sectors = kernel_disk.sectors;
    *most = (kernel_disk.most != 0 ? kernel_disk.most : KFS_BLOCK) / KFS_BLOCK;
    return true;
}

static int stick_io(uint32_t op, uint32_t block, uint32_t count)
{
    struct block_reply r;
    uint32_t error;
    uint64_t sector = (uint64_t)block * KFS_PER_BLOCK;
    uint32_t sectors = count * KFS_PER_BLOCK;

    if (!stick_found() || sector + sectors > stick.sectors
        || (uint64_t)count * KFS_BLOCK > STICK_MOST) {
        return -1;
    }

    return ask(blocks_write, op, stick.unit, stick.first + sector, sectors, -1,
               &r, &error) ? 0 : -1;
}

static int device_read(void *ctx, uint32_t block, uint32_t count, void *to)
{
    uint64_t began = kosmos_ticks();
    size_t bytes = (size_t)count * KFS_BLOCK;
    int r;

    (void)ctx;

    if (stick.use) {
        r = stick_io(BLOCK_OP_READ, block, count);

        if (r == 0) {
            memcpy(to, stick.at, bytes);
        }
    } else {
        long got = kosmos_disk_read((unsigned long)block * KFS_PER_BLOCK, to, bytes);

        r = (got == (long)bytes) ? 0 : -1;
    }

    cost.read_counter_ticks += kosmos_ticks() - began;
    cost.reads++;
    cost.read_bytes += (r == 0) ? bytes : 0;
    return r;
}

static int device_write(void *ctx, uint32_t block, uint32_t count, const void *from)
{
    uint64_t began = kosmos_ticks();
    size_t bytes = (size_t)count * KFS_BLOCK;
    int r;

    (void)ctx;

    if (stick.use) {
        memcpy(stick.at, from, bytes);
        r = stick_io(BLOCK_OP_WRITE, block, count);

        /*
         * The journal's header is the commit, and a stick holds writes in a
         * cache of its own: asked to write it out every time, and the
         * driver remembers a stick that has said it cannot (the Lua server
         * has why).
         */
        if (r == 0 && get_le32(from) == KFS_J_MAGIC) {
            struct block_reply f;
            uint32_t error;

            if (!ask(blocks_write, BLOCK_OP_FLUSH, stick.unit, 0, 0, -1, &f, &error)
                && (error == BLOCK_ERR_NO_FLUSH || stick.flush_why[0] == '\0')) {
                put_text(stick.flush_why, sizeof stick.flush_why,
                         error == BLOCK_ERR_NO_FLUSH
                         ? "the stick does not do SYNCHRONIZE CACHE"
                         : "the stick failed it, or did not answer");
            }
        }
    } else {
        long wrote = kosmos_disk_write((unsigned long)block * KFS_PER_BLOCK, from, bytes);

        r = (wrote == (long)bytes) ? 0 : -1;
    }

    cost.write_counter_ticks += kosmos_ticks() - began;
    cost.writes++;
    cost.write_bytes += bytes;
    return r;
}

/*
 * ------------------------------------------------------------------------
 * The filesystem, mounted on the first request that needs it.
 * ------------------------------------------------------------------------
 */

/* A disk of all zeros has nothing to lose, and formats itself once. */
static bool blank(void)
{
    if (kfs_read_block(&A->kfs, 0, A->packed) != KFS_OK) {
        return false;
    }

    for (uint32_t i = 0; i < KFS_BLOCK; i++) {
        if (A->packed[i] != 0) {
            return false;
        }
    }

    return true;
}

static bool mounted(void)
{
    uint64_t sectors;
    uint32_t most, replayed;
    const char *why;

    if (mounted_ok) {
        return true;
    }

    if (!device(&sectors, &most, &why)) {
        return false;
    }

    A->kfs.disk.most = most;

    if (kfs_mount(&A->kfs, &SB) != KFS_OK) {
        if (blank()) {
            int r = kfs_mkfs(&A->kfs, sectors, stamp(), NULL, &SB);

            if (r != KFS_OK) {
                tell("it is blank and would not format: ", 0, false, kfs_why(r));
                return false;
            }

            tell("it was blank, so it has been formatted", 0, false, NULL);
        } else {
            if (!told_unreadable) {
                told_unreadable = true;
                tell("there is something on this disk that is not a filesystem "
                     "this understands, so it has been left alone", 0, false, NULL);
            }

            return false;
        }
    }

    /* A write in progress when the power went, finished - and said. */
    if (kfs_recover(&A->kfs, &SB, &replayed) == KFS_OK && replayed > 0) {
        tell("the last write did not finish; ", replayed, true, " block(s) replayed");
    }

    mounted_ok = true;
    return true;
}

/*
 * One operation, all of it or none of it: `r` is what `call` said, or the
 * commit's refusal. Everything that changes the disk goes through here.
 */
#define ATOMIC(r, call)                                                      \
    do {                                                                     \
        (r) = kfs_begin(&A->kfs);                                            \
                                                                             \
        if ((r) == KFS_OK) {                                                 \
            (r) = (call);                                                    \
                                                                             \
            if ((r) == KFS_OK) {                                             \
                (r) = kfs_commit(&A->kfs, &SB, false);                       \
            } else {                                                         \
                kfs_rollback(&A->kfs);                                       \
            }                                                                \
        }                                                                    \
    } while (0)

static uint32_t kfs_error(int r)
{
    return DISK_ERR_KFS + (uint32_t)(-r);
}

/* The last part of a path, and its length; none for the root. */
static const char *last_name(const char *path, size_t *len)
{
    const char *end = path + strlen(path);
    const char *at;

    while (end > path && end[-1] == '/') {
        end--;
    }

    at = end;

    while (at > path && at[-1] != '/') {
        at--;
    }

    *len = (size_t)(end - at);
    return *len > 0 ? at : NULL;
}

static bool reserved(const char *name, size_t len)
{
    if (name == NULL) {
        return true;
    }

    for (size_t i = 0; i < sizeof RESERVED / sizeof RESERVED[0]; i++) {
        if (kfs_same_name(name, len, RESERVED[i], strlen(RESERVED[i]))) {
            return true;
        }
    }

    return false;
}

/*
 * ------------------------------------------------------------------------
 * The operations.
 * ------------------------------------------------------------------------
 */

static int order_by_name(const void *a, const void *b)
{
    const uint8_t *x = A->kfs.dir + *(const uint32_t *)a;
    const uint8_t *y = A->kfs.dir + *(const uint32_t *)b;
    uint32_t xn = x[4], yn = y[4];
    int c = memcmp(x + 5, y + 5, xn < yn ? xn : yn);

    return c != 0 ? c : (int)xn - (int)yn;
}

static void op_list(const struct disk_request *rq, struct disk_reply *rp)
{
    uint32_t pos = 0, here, inode, n, count = 0, used = 0;
    const char *name;
    int r = kfs_list(&A->kfs, &SB, rq->path, strlen(rq->path));

    if (r != KFS_OK) {
        rp->error = kfs_error(r);
        return;
    }

    while (here = pos, kfs_dir_next(&A->kfs, &pos, &inode, &name, &n)) {
        if (count == NAMES_MAX) {
            rp->error = kfs_error(KFS_E_DIR_BIG);
            return;
        }

        A->order[count++] = here;
    }

    qsort(A->order, count, sizeof A->order[0], order_by_name);

    for (uint64_t i = rq->offset; i < count; i++) {
        const uint8_t *e = A->kfs.dir + A->order[i];

        if (used + e[4] + 1u > DISK_DATA_MAX) {
            rp->more = 1;
            rp->offset = i;
            break;
        }

        memcpy(rp->u.data + used, e + 5, e[4]);
        used += e[4];
        rp->u.data[used++] = '\0';
        rp->count++;
    }

    rp->length = used;
}

/* The caller's region, mapped for this request: its address and size. */
struct region {
    long cap;
    uint8_t *at;
    uint64_t bytes;
};

static bool map_region(long cap, struct region *out)
{
    long pages, at;

    out->cap = cap;
    out->at = NULL;

    if (cap < 0 || (pages = kosmos_mem_size(cap)) <= 0
        || (at = kosmos_mem_map(cap)) < 0) {
        return false;
    }

    out->at = (uint8_t *)(uintptr_t)at;
    out->bytes = (uint64_t)pages * 4096u;
    return true;
}

static void unmap_region(struct region *r)
{
    if (r->at != NULL) {
        kosmos_share_unmap((unsigned long)(uintptr_t)r->at,
                           (unsigned long)(r->bytes / 4096u));
        r->at = NULL;
    }
}

static void op_read(const struct disk_request *rq, struct disk_reply *rp, long cap)
{
    struct kfs_inode node;
    uint32_t number;
    uint64_t placed, want;
    size_t len;
    const char *name = last_name(rq->path, &len);
    int r;

    if (!(rq->flags & DISK_REGION) && name == NULL) {
        rp->error = DISK_ERR_THE_DIRECTORY;
        return;
    }

    r = kfs_find(&A->kfs, &SB, rq->path, strlen(rq->path), &number, &node);

    if (r != KFS_OK) {
        rp->error = kfs_error(r);
        return;
    }

    if (node.kind == KFS_KIND_DIR) {
        rp->error = kfs_error(KFS_E_IS_DIR);
        return;
    }

    rp->size = node.size;
    want = rq->offset < node.size ? node.size - rq->offset : 0;

    if (rq->flags & DISK_REGION) {
        struct region region;

        if (rq->bytes < want) {
            want = rq->bytes;
        }

        if (!map_region(cap, &region) || want > region.bytes) {
            unmap_region(&region);
            rp->error = DISK_ERR_REGION;
            return;
        }

        r = kfs_read_range(&A->kfs, &SB, &node, rq->offset, want, region.at, &placed);
        unmap_region(&region);

        if (r != KFS_OK) {
            rp->error = kfs_error(r);
            return;
        }

        rp->bytes = placed;
        return;
    }

    if (want > DISK_DATA_MAX) {
        want = DISK_DATA_MAX;
        rp->more = 1;
    }

    r = kfs_read_range(&A->kfs, &SB, &node, rq->offset, want, rp->u.data, &placed);

    if (r != KFS_OK) {
        rp->error = kfs_error(r);
        return;
    }

    rp->length = (uint32_t)placed;
    rp->offset = rq->offset + placed;
}

static void op_write(const struct disk_request *rq, struct disk_reply *rp, long cap)
{
    uint32_t number;
    size_t len;
    const char *name = last_name(rq->path, &len);
    int r;

    if (reserved(name, len)) {
        rp->error = DISK_ERR_RESERVED;
        return;
    }

    if (rq->flags & DISK_REGION) {
        struct region region;

        if (!map_region(cap, &region) || rq->bytes > region.bytes) {
            unmap_region(&region);
            rp->error = DISK_ERR_REGION;
            return;
        }

        ATOMIC(r, kfs_store(&A->kfs, &SB, rq->path, strlen(rq->path), region.at,
                            rq->bytes, stamp(), &number));
        unmap_region(&region);
        rp->bytes = rq->bytes;
    } else {
        if (rq->length > DISK_DATA_MAX) {
            rp->error = DISK_ERR_BAD_OP;
            return;
        }

        ATOMIC(r, kfs_store(&A->kfs, &SB, rq->path, strlen(rq->path), rq->u.data,
                            rq->length, stamp(), &number));
        rp->bytes = rq->length;
    }

    if (r != KFS_OK) {
        rp->error = kfs_error(r);
    }
}

static void op_delete(const struct disk_request *rq, struct disk_reply *rp)
{
    size_t len;
    const char *name = last_name(rq->path, &len);
    int r;

    if (reserved(name, len)) {
        rp->error = DISK_ERR_RESERVED;
        return;
    }

    ATOMIC(r, kfs_unlink(&A->kfs, &SB, rq->path, strlen(rq->path)));

    if (r != KFS_OK) {
        rp->error = kfs_error(r);
    }
}

static void op_rename(const struct disk_request *rq, struct disk_reply *rp)
{
    size_t len, to_len;
    const char *name = last_name(rq->path, &len);
    const char *to_name;
    int r;

    if (rq->length == 0 || rq->length >= DISK_PATH_MAX
        || memchr(rq->u.to, '\0', rq->length + 1) != rq->u.to + rq->length) {
        rp->error = DISK_ERR_BAD_OP;
        return;
    }

    to_name = last_name(rq->u.to, &to_len);

    if (reserved(name, len) || reserved(to_name, to_len)) {
        rp->error = DISK_ERR_RESERVED;
        return;
    }

    ATOMIC(r, kfs_rename(&A->kfs, &SB, rq->path, strlen(rq->path), rq->u.to,
                         rq->length));

    if (r != KFS_OK) {
        rp->error = kfs_error(r);
    }
}

static void op_mkdir(const struct disk_request *rq, struct disk_reply *rp)
{
    int r;

    ATOMIC(r, kfs_mkdir(&A->kfs, &SB, rq->path, strlen(rq->path), stamp()));

    if (r != KFS_OK) {
        rp->error = kfs_error(r);
    }
}

static void node_facts(const struct kfs_inode *node, struct disk_node *out)
{
    uint64_t epoch;

    out->kind = node->kind;
    out->extents = node->extents;
    out->size = node->size;
    out->mtime = node->mtime;
    out->dated = kfs_modified(node->mtime, &epoch) ? 1u : 0u;
    out->modified = out->dated ? epoch : 0;
}

static void op_getattr(const struct disk_request *rq, struct disk_reply *rp)
{
    struct kfs_inode node;
    uint32_t number, len32;
    size_t len;
    const char *name = last_name(rq->path, &len);
    int r;

    if (reserved(name, len)) {
        rp->node.kind = DISK_KIND_DEVICE;
        return;
    }

    if (!mounted()) {
        rp->error = DISK_ERR_NO_FS;
        return;
    }

    r = kfs_find(&A->kfs, &SB, rq->path, strlen(rq->path), &number, &node);

    if (r == KFS_OK) {
        r = kfs_read_attrs(&A->kfs, &SB, &node, A->attrs, &len32);
    }

    if (r != KFS_OK) {
        rp->error = kfs_error(r);
        return;
    }

    node_facts(&node, &rp->node);

    if (rq->offset < len32) {
        uint64_t n = len32 - rq->offset;

        if (n > DISK_DATA_MAX) {
            n = DISK_DATA_MAX;
            rp->more = 1;
        }

        memcpy(rp->u.data, A->attrs + rq->offset, (size_t)n);
        rp->length = (uint32_t)n;
        rp->offset = rq->offset + n;
    }
}

/* Whether a key is the string `name`. */
static bool key_is(const struct packflat_value *k, const char *name)
{
    size_t n = strlen(name);

    return k->type == PACKFLAT_STRING && k->len == n && memcmp(k->text, name, n) == 0;
}

/* What a node is, which is not stored: refused as an attribute to set. */
static bool derived(const struct packflat_value *k, const struct kfs_inode *node)
{
    return key_is(k, "size") || key_is(k, "mtime") || key_is(k, "extents")
           || (key_is(k, "kind") && node->kind == KFS_KIND_DIR);
}

/* The stored attributes and the changes, merged: "" takes a name out. */
static int merge_attrs(const struct kfs_super *sb, uint32_t number,
                       struct kfs_inode *node)
{
    size_t len;
    int r;

    for (uint32_t i = 0; i < A->terms.count; i++) {
        const struct packflat_value *k = &A->terms.key[i];
        const struct packflat_value *v = &A->terms.value[i];

        if (k->type != PACKFLAT_STRING) {
            return KFS_E_NOT_ATTRS;
        }

        r = packflat_set(&A->stored, k->text, k->len,
                         (v->type == PACKFLAT_STRING && v->len == 0) ? NULL : v);

        if (r != PACKFLAT_OK) {
            return KFS_E_ATTRS_BIG;
        }
    }

    if (packflat_write(&A->stored, A->packed, KFS_BLOCK - 4, &len) != PACKFLAT_OK) {
        return KFS_E_ATTRS_BIG;
    }

    /* An empty table is no attributes at all, and gives its block back. */
    return kfs_write_attrs(&A->kfs, sb, number, node, A->packed,
                           A->stored.count == 0 ? 0 : (uint32_t)len);
}

static void op_setattr(const struct disk_request *rq, struct disk_reply *rp)
{
    struct kfs_inode node;
    uint32_t number, len32;
    size_t len;
    const char *name = last_name(rq->path, &len);
    int r;

    if (reserved(name, len)) {
        rp->error = DISK_ERR_RESERVED;
        return;
    }

    if (rq->length > DISK_DATA_MAX
        || packflat_read(rq->u.data, rq->length, &A->terms) != PACKFLAT_OK) {
        rp->error = DISK_ERR_NOT_FLAT;
        return;
    }

    r = kfs_find(&A->kfs, &SB, rq->path, strlen(rq->path), &number, &node);

    if (r == KFS_OK) {
        r = kfs_read_attrs(&A->kfs, &SB, &node, A->attrs, &len32);
    }

    if (r != KFS_OK) {
        rp->error = kfs_error(r);
        return;
    }

    /* Refused whole, and before anything is written (the Lua server has
     * why). */
    for (uint32_t i = 0; i < A->terms.count; i++) {
        const struct packflat_value *k = &A->terms.key[i];

        if (derived(k, &node)) {
            uint32_t n = k->len < DISK_DATA_MAX ? k->len : DISK_DATA_MAX;

            memcpy(rp->u.data, k->text, n);
            rp->length = n;
            rp->error = DISK_ERR_DERIVED;
            return;
        }
    }

    if (len32 == 0) {
        A->stored.count = 0;
    } else if (packflat_read(A->attrs, len32, &A->stored) != PACKFLAT_OK) {
        rp->error = DISK_ERR_NOT_FLAT;
        return;
    }

    ATOMIC(r, merge_attrs(&SB, number, &node));

    if (r != KFS_OK) {
        rp->error = r == KFS_E_ATTRS_BIG ? DISK_ERR_ATTRS_BIG : kfs_error(r);
    }
}

/*
 * ------------------------------------------------------------------------
 * A query: everything under the path asked about whose attributes are the
 * terms' values, compared as Lua's `tostring` writes each - what the index
 * compared - with the node's facts over what is stored: its kind, size,
 * stamp, date, extents and name.
 * ------------------------------------------------------------------------
 */

static size_t fact_text(const char *key, uint32_t key_len,
                        const struct kfs_inode *node, const char *name,
                        size_t name_len, bool *known, char *out)
{
    struct packflat_value v;
    uint64_t epoch;

#define IS(word) (key_len == sizeof(word) - 1 && memcmp(key, word, key_len) == 0)
    *known = true;

    if (IS("size")) {
        v = packflat_int((int64_t)node->size);
    } else if (IS("mtime")) {
        v = packflat_int((int64_t)node->mtime);
    } else if (IS("extents")) {
        v = packflat_int((int64_t)node->extents);
    } else if (IS("modified")) {
        if (!kfs_modified(node->mtime, &epoch)) {
            memcpy(out, "nil", 3);
            return 3;
        }

        v = packflat_int((int64_t)epoch);
    } else if (IS("name")) {
        v = packflat_string(name, name_len);
    } else if (IS("kind") && node->kind == KFS_KIND_DIR) {
        v = packflat_string("directory", 9);
    } else {
        *known = false;
        return 0;
    }
#undef IS

    return packflat_text(&v, out, 64 + KFS_NAME_MAX);
}

/*
 * Whether a node answers every term. **What is stored is read only when a
 * term asks for it**: a query by name, size or kind of folder never reads an
 * attribute block, which was a disk read for every file that had one.
 */
static bool matches(const struct kfs_inode *node, const char *name, size_t name_len)
{
    char want[DISK_DATA_MAX + 1], have[DISK_DATA_MAX + 1];
    uint32_t len32;
    bool stored = false, read = false;

    for (uint32_t i = 0; i < A->terms.count; i++) {
        const struct packflat_value *k = &A->terms.key[i];
        const struct packflat_value *v;
        size_t want_len = packflat_text(&A->terms.value[i], want, DISK_DATA_MAX);
        size_t have_len;
        bool known;

        have_len = fact_text(k->text, k->len, node, name, name_len, &known, have);

        if (!known && !read) {
            read = true;
            stored = node->attrs != 0
                     && kfs_read_attrs(&A->kfs, &SB, node, A->attrs, &len32) == KFS_OK
                     && len32 > 0
                     && packflat_read(A->attrs, len32, &A->stored) == PACKFLAT_OK;
        }

        if (!known) {
            v = stored ? packflat_get(&A->stored, k->text, k->len) : NULL;

            if (v != NULL) {
                have_len = packflat_text(v, have, DISK_DATA_MAX);
            } else if (k->len == 4 && memcmp(k->text, "kind", 4) == 0) {
                memcpy(have, "file", 4);
                have_len = 4;
            } else {
                memcpy(have, "nil", 3);
                have_len = 3;
            }
        }

        if (have_len != want_len || memcmp(have, want, want_len) != 0) {
            return false;
        }
    }

    return true;
}

/* A path onto the query's list of folders still to visit. */
static bool push_folder(uint32_t *top, uint32_t inode, const char *path, size_t len)
{
    if (*top + 6 + len > PENDING_BYTES) {
        return false;
    }

    memcpy(A->pending + *top, path, len);
    *top += (uint32_t)len;
    memcpy(A->pending + *top, &inode, 4);
    *top += 4;
    A->pending[(*top)++] = (uint8_t)(len & 0xff);
    A->pending[(*top)++] = (uint8_t)(len >> 8);
    return true;
}

static void pop_folder(uint32_t *top, uint32_t *inode, char *path, size_t *len)
{
    *len = (size_t)A->pending[*top - 2] | (size_t)A->pending[*top - 1] << 8;
    *top -= 2;
    memcpy(inode, A->pending + *top - 4, 4);
    *top -= 4;
    *top -= (uint32_t)*len;
    memcpy(path, A->pending + *top, *len);
    path[*len] = '\0';
}

/* A match, kept with the answer; false when the answer is full. */
static bool answer_path(const char *path, size_t len)
{
    if (A->found_len + len + 1 > FOUND_BYTES) {
        return false;
    }

    memcpy(A->found + A->found_len, path, len);
    A->found_len += (uint32_t)len;
    A->found[A->found_len++] = '\0';
    A->found_count++;
    return true;
}

/* A page of the kept answer, from its `offset`th path. */
static void answer_page(const struct disk_request *rq, struct disk_reply *rp)
{
    uint32_t at = 0;

    for (uint64_t i = 0; i < rq->offset && at < A->found_len; i++) {
        at += (uint32_t)strlen(A->found + at) + 1;
    }

    for (uint64_t i = rq->offset; at < A->found_len; i++) {
        uint32_t n = (uint32_t)strlen(A->found + at) + 1;

        if (rp->length + n > DISK_DATA_MAX) {
            rp->more = 1;
            rp->offset = i;
            return;
        }

        memcpy(rp->u.data + rp->length, A->found + at, n);
        rp->length += n;
        rp->count++;
        at += n;
    }
}

/* The whole answer to the terms in `A->terms` under `rq->path`, kept. */
static uint32_t scan(const struct disk_request *rq)
{
    struct kfs_inode node;
    uint32_t number, top = 0;
    size_t len, name_len;
    const char *name;
    char *path = A->path;
    int r;

    A->found_len = A->found_count = 0;
    r = kfs_find(&A->kfs, &SB, rq->path, strlen(rq->path), &number, &node);

    if (r != KFS_OK) {
        return kfs_error(r);
    }

    /* The disk's spelling of where it starts, which every answer begins with. */
    len = kfs_spelled(&A->kfs, &SB, rq->path, strlen(rq->path), false, path);

    while (len > 0 && path[len - 1] == '/') {
        len--;
    }

    path[len] = '\0';
    name = last_name(path, &name_len);

    if (name != NULL && matches(&node, name, name_len) && !answer_path(path, len)) {
        return DISK_ERR_ANSWERS;
    }

    if (node.kind != KFS_KIND_DIR) {
        return DISK_OK;
    }

    if (!push_folder(&top, number, path, len)) {
        return DISK_ERR_SEARCH;
    }

    while (top > 0) {
        uint32_t pos = 0, inode, n, folder;
        size_t base;
        const char *entry;

        pop_folder(&top, &folder, path, &base);

        if (kfs_read_inode(&A->kfs, &SB, folder, &node) != KFS_OK
            || kfs_open_dir(&A->kfs, &SB, &node) != KFS_OK) {
            continue;
        }

        while (kfs_dir_next(&A->kfs, &pos, &inode, &entry, &n)) {
            struct kfs_inode child;

            if (base + 1 + n >= DISK_PATH_MAX * 2) {
                continue;
            }

            path[base] = '/';
            memcpy(path + base + 1, entry, n);
            path[base + 1 + n] = '\0';

            if (kfs_read_inode(&A->kfs, &SB, inode, &child) != KFS_OK
                || child.kind == KFS_KIND_FREE) {
                continue;
            }

            if (matches(&child, entry, n) && !answer_path(path, base + 1 + n)) {
                return DISK_ERR_ANSWERS;
            }

            if (child.kind == KFS_KIND_DIR
                && !push_folder(&top, inode, path, base + 1 + n)) {
                return DISK_ERR_SEARCH;
            }
        }
    }

    return DISK_OK;
}

static void op_query(const struct disk_request *rq, struct disk_reply *rp)
{
    size_t plen = strlen(rq->path);
    uint32_t key_len = (uint32_t)(plen + 1 + rq->length);
    bool same;

    if (rq->length > DISK_DATA_MAX
        || packflat_read(rq->u.data, rq->length, &A->terms) != PACKFLAT_OK) {
        rp->error = DISK_ERR_NOT_FLAT;
        return;
    }

    for (uint32_t i = 0; i < A->terms.count; i++) {
        if (A->terms.key[i].type != PACKFLAT_STRING) {
            rp->error = DISK_ERR_NOT_FLAT;
            return;
        }
    }

    if (A->terms.count == 0) {
        return;
    }

    /* The same question of the same disk, past its first page: kept. */
    same = A->found_valid && rq->offset > 0 && A->found_generation == generation
           && A->found_key_len == key_len
           && memcmp(A->found_key, rq->path, plen + 1) == 0
           && memcmp(A->found_key + plen + 1, rq->u.data, rq->length) == 0;

    if (!same) {
        uint32_t r = scan(rq);

        A->found_valid = r == DISK_OK;

        if (r != DISK_OK) {
            rp->error = r;
            return;
        }

        memcpy(A->found_key, rq->path, plen + 1);
        memcpy(A->found_key + plen + 1, rq->u.data, rq->length);
        A->found_key_len = key_len;
        A->found_generation = generation;
    }

    answer_page(rq, rp);
}

/*
 * ------------------------------------------------------------------------
 * The disk itself.
 * ------------------------------------------------------------------------
 */

static void op_super(struct disk_reply *rp)
{
    struct disk_super *s = &rp->u.super;
    uint64_t sectors;
    uint32_t most;
    const char *why;

    s->searched = stick.use ? 1u : 0u;
    s->looks = stick.looks;
    memcpy(s->stops, stick.stops, sizeof s->stops);
    s->first = stick.first_look;
    s->found = stick.found_at;
    put_text(s->where, sizeof s->where, stick.where);
    put_text(s->flush_why, sizeof s->flush_why, stick.flush_why);

    if (!device(&sectors, &most, &why)) {
        put_text(s->why, sizeof s->why, why);
        return;
    }

    s->present = 1;
    s->sectors = sectors;
    s->sector_size = stick.use ? SECTOR : kernel_disk.sector_size;
    s->bytes = sectors * s->sector_size;

    if (!mounted()) {
        put_text(s->why, sizeof s->why, "not a kosmos filesystem");
        return;
    }

    s->formatted = 1;
    s->magic = SB.magic;
    s->version = SB.version;
    s->block_size = SB.block_size;
    s->blocks = SB.blocks;
    s->bitmap_at = SB.bitmap_at;
    s->bitmap_blocks = SB.bitmap_blocks;
    s->inodes_at = SB.inodes_at;
    s->inode_count = SB.inode_count;
    s->journal_at = SB.journal_at;
    s->data_at = SB.data_at;
    s->created = SB.created;

    /* Counted out of the bitmap; absent rather than zero when it will not read. */
    if (kfs_free_blocks(&A->kfs, &SB, &s->free_blocks) == KFS_OK) {
        s->free_known = 1;
    } else {
        put_text(s->free_why, sizeof s->free_why, "the bitmap would not read");
    }
}

static void op_device(struct disk_reply *rp)
{
    rp->u.device = cost;
    rp->u.device.cache_hits = A->cache.hits;
    rp->u.device.cache_misses = A->cache.misses;
}

static void op_format(const struct disk_request *rq, struct disk_reply *rp)
{
    static const char words[] = "yes, erase it";
    uint64_t sectors;
    uint32_t most;
    const char *why;
    int r;

    if (rq->length != sizeof words - 1 || memcmp(rq->u.data, words, rq->length) != 0) {
        rp->error = DISK_ERR_WORDS;
        return;
    }

    if (!device(&sectors, &most, &why)) {
        rp->error = DISK_ERR_NO_DISK;
        put_text(rp->u.data, DISK_DATA_MAX, why);
        rp->length = (uint32_t)strlen(rp->u.data);
        return;
    }

    A->kfs.disk.most = most;
    diskcache_clear(&A->cache);
    r = kfs_mkfs(&A->kfs, sectors, stamp(), NULL, &SB);

    if (r != KFS_OK) {
        mounted_ok = false;
        rp->error = kfs_error(r);
        return;
    }

    mounted_ok = true;
    op_super(rp);
}

/*
 * ------------------------------------------------------------------------
 * The loop.
 * ------------------------------------------------------------------------
 */

static void answer(const struct message *msg, uint64_t sender)
{
    static struct message out;
    const struct disk_request *rq = (const struct disk_request *)msg->data;
    struct disk_reply *rp = (struct disk_reply *)out.data;
    long cap = msg->cap_plus_one ? (long)msg->cap_plus_one - 1 : -1;

    memset(&out, 0, sizeof out);
    out.length = sizeof *rp;

    if (msg->length != sizeof *rq || memchr(rq->path, '\0', DISK_PATH_MAX) == NULL) {
        rp->error = DISK_ERR_BAD_OP;
    } else if (rq->op == DISK_OP_SUPER) {
        op_super(rp);
    } else if (rq->op == DISK_OP_DEVICE) {
        op_device(rp);
    } else if (rq->op == DISK_OP_FORMAT) {
        op_format(rq, rp);
    } else if (rq->op == DISK_OP_GETATTR) {
        op_getattr(rq, rp);
    } else if (!mounted()) {
        rp->error = DISK_ERR_NO_FS;
    } else {
        switch (rq->op) {
        case DISK_OP_LIST:    op_list(rq, rp); break;
        case DISK_OP_READ:    op_read(rq, rp, cap); break;
        case DISK_OP_WRITE:   op_write(rq, rp, cap); break;
        case DISK_OP_DELETE:  op_delete(rq, rp); break;
        case DISK_OP_RENAME:  op_rename(rq, rp); break;
        case DISK_OP_MKDIR:   op_mkdir(rq, rp); break;
        case DISK_OP_SETATTR: op_setattr(rq, rp); break;
        case DISK_OP_QUERY:   op_query(rq, rp); break;
        default:              rp->error = DISK_ERR_BAD_OP; break;
        }
    }

    /* Anything that may have changed the disk makes a kept answer stale. */
    if (rq->op == DISK_OP_WRITE || rq->op == DISK_OP_DELETE || rq->op == DISK_OP_RENAME
        || rq->op == DISK_OP_MKDIR || rq->op == DISK_OP_SETATTR
        || rq->op == DISK_OP_FORMAT) {
        generation++;
    }

    /* A region handed over is given back on every path, answered or not. */
    if (cap >= 0) {
        kosmos_cap_drop(cap);
    }

    (void)kosmos_reply(sender, &out);
}

void diskfs_server(long endpoint, long read_ep, long write_ep, long devices_ep,
                   long console_ep)
{
    long at = kosmos_map((sizeof(struct arena) + 4095u) / 4096u);
    struct kfs_disk dev = { NULL, device_read, device_write, 1 };
    char home[64];
    long n;

    blocks_read = read_ep;
    blocks_write = write_ep;
    devices = devices_ep;
    console = console_ep;

    if (at < 0) {
        say(console, "diskfs: no memory to work in\n");
        return;
    }

    A = (struct arena *)(uintptr_t)at;

    /* `/Home` on a stick: `usb` for the first Kosmos partition, or one by GUID. */
    n = kosmos_boot_option("opt/kosmos/home", home, sizeof home - 1);

    if (n > 0) {
        home[n < (long)sizeof home ? n : (long)sizeof home - 1] = '\0';

        if (strcmp(home, "usb") == 0) {
            stick.use = true;
        } else if (strlen(home) == 36) {
            stick.use = true;
            stick.have_wanted = true;

            for (int i = 0; i < 36; i++) {
                char c = home[i];

                stick.wanted[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
            }

            stick.wanted[36] = '\0';
        }
    }

    diskcache_init(&A->cache, &dev, DISKCACHE_MOST, 4);
    kfs_init(&A->kfs, &(struct kfs_disk){ 0 });
    A->kfs.disk = diskcache_disk(&A->cache);

    for (;;) {
        static struct message msg;
        uint64_t sender = 0;

        if (kosmos_receive(endpoint, &msg, &sender, 0, 0) != 0) {
            return;
        }

        answer(&msg, sender);
    }
}
