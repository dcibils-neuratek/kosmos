/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /drives: every volume on every drive, read only (USB step 6b,
 * `docs/drives.html`).
 *
 * **One server owns the whole prefix.** `drives.html` says `/drives` is "one
 * folder every program has from the moment it starts", with drives appearing
 * and disappearing inside it while programs run. A mount is an entry in a
 * process's own namespace, made when that process is built, so a volume
 * appearing later would mean editing the namespace of every running program -
 * which nothing can do. A server behind one prefix needs none of that: the
 * matcher routes `/drives/PHOTOS 2024/Italy` here with the rest of the path
 * intact, exactly as `/home` is routed to the disk server today.
 *
 * **It writes nothing, and that is structural rather than promised.** init
 * gives this process the USB driver's *read* endpoint and never the write
 * one, so a write is not something this server declines - it is something it
 * cannot express. `drivesproto.h` has no operation for one either.
 *
 * **What it knows about bytes it does not know itself.** `fat_decode.c` says
 * what a FAT volume's sectors mean and `drives_decode.c` says where the
 * volumes are and what they are called; both are pure and are tested on the
 * Mac. What is here is the part that cannot be: asking the USB driver for
 * sectors, and answering messages.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kosmos.h"
#include "blockproto.h"
#include "drivesproto.h"
#include "drives_decode.h"
#include "fat_decode.h"
#include "storage_decode.h"
#include "say.h"

/* The two halves of the volume list must agree about how long a name is, and
 * they are written down twice because a client has no business including the
 * partition-table reader. */
_Static_assert((unsigned)DRIVES_NAME_BYTES == (unsigned)64u,
               "drivesproto.h and drives_decode.h disagree about a name");

/*
 * `enum drives_fs` travels as a number, so the two orders must match.
 *
 * Written twice on purpose - a client reads `drivesproto.h` and has no
 * business including the partition-table reader - and therefore checked,
 * because a value that drifts would put every volume under the wrong
 * filesystem name with nothing failing to say so.
 */
_Static_assert((unsigned)DRIVES_FS_NONE  == (unsigned)FS_KIND_NONE
               && (unsigned)DRIVES_FS_FAT16 == (unsigned)FS_KIND_FAT16
               && (unsigned)DRIVES_FS_FAT32 == (unsigned)FS_KIND_FAT32
               && (unsigned)DRIVES_FS_KFS   == (unsigned)FS_KIND_KFS
               && (unsigned)DRIVES_FS_OTHER == (unsigned)FS_KIND_OTHER,
               "drivesproto.h and drives_decode.h disagree about a "
               "filesystem's number");

#define SECTOR              512u
#define VOLUMES_MOST        16u     /* volumes this server will hold */
#define REGION_PAGES        (BLOCK_TRANSFER_MOST / 4096u)
#define REGION_SECTORS      (BLOCK_TRANSFER_MOST / SECTOR)

/*
 * How long a scan's answer is kept before the drives are looked at again.
 *
 * A listing that rescanned every drive on every message would read a
 * partition table to answer "what is in this directory", and Tracker asks
 * that on every keystroke of a search. A scan that never repeated would never
 * see a stick plugged in. Two seconds is under the time it takes a person to
 * plug something in and look, and over the time a window takes to draw
 * itself.
 */
#define RESCAN_TICKS        (2u * 250u)     /* TICK_HZ is 250 */

struct volume {
    char     name[DRIVES_NAME_BYTES];
    enum drives_fs fs;
    uint32_t unit;
    uint32_t partition;
    uint64_t first;             /* its first sector, on the drive */
    uint64_t sectors;
    uint64_t bytes;
    uint64_t free_bytes;
    bool     free_exact;
    bool     readable;          /* whether this server can open it */
    struct fat_volume fat;      /* when `fs` is FAT16 or FAT32 */
};

static long blocks = -1;        /* the USB driver's read endpoint */
static long console = -1;
static long region_cap = -1;
static uint8_t *region;         /* where the driver's bytes arrive */
static uint32_t handle;         /* what BLOCK_OP_OPEN answered */

static struct volume volumes[VOLUMES_MOST];
static unsigned volume_count;
static unsigned long scanned_at;        /* ticks, 0 for never */

static void *zero(void *p, unsigned long n)
{
    uint8_t *b = (uint8_t *)p;

    while (n-- > 0u) {
        *b++ = 0u;
    }

    return p;
}

static void copy(void *to, const void *from, unsigned long n)
{
    uint8_t *d = (uint8_t *)to;
    const uint8_t *s = (const uint8_t *)from;

    while (n-- > 0u) {
        *d++ = *s++;
    }
}

static unsigned len_of(const char *s)
{
    unsigned n = 0;

    while (s[n] != '\0') {
        n++;
    }

    return n;
}

/*
 * One request to the USB driver.
 *
 * `pass` is a capability travelling with it, which only `BLOCK_OP_OPEN` uses:
 * the region is created here, sent once, and the driver copies sectors into
 * it. **Control by message, data by shared memory** - a read says which
 * blocks and the bytes arrive in the region, never in the reply.
 */
static bool blocks_call(uint32_t op, uint32_t unit, uint64_t lba,
                        uint32_t count, long pass, struct block_reply *out)
{
    struct message msg, rep;
    struct block_request *req = (struct block_request *)msg.data;

    if (blocks < 0) {
        return false;
    }

    zero(&msg, sizeof(msg));
    msg.length = sizeof(*req);
    msg.cap_plus_one = (pass >= 0) ? (uint32_t)(pass + 1) : 0u;

    req->op = op;
    req->unit = unit;
    req->lba = lba;
    req->count = count;
    req->handle = handle;

    if (kosmos_call(blocks, &msg, &rep) != 0) {
        return false;
    }

    if (rep.length < sizeof(struct block_reply)) {
        return false;
    }

    copy(out, rep.data, sizeof(*out));

    return out->error == BLOCK_OK;
}

/* How many unit numbers the driver has given out. A walk goes up to this and
 * steps over the gaps sticks that have left make (`blockproto.h`). */
static unsigned units(void)
{
    struct block_reply rep;

    zero(&rep, sizeof(rep));

    /*
     * **The answer is read whether or not the call succeeded, and that is
     * why `rep` is cleared first.** An INFO about unit 0 fails on a machine
     * whose first stick has been pulled, and `count` is still the number of
     * units given out - `blockproto.h` says so. Both branches therefore read
     * the field, and the first version of this returned it from a `rep` that
     * a failed call had never filled: stack garbage as a unit count, which
     * scans nothing or loops over billions, and compiles perfectly.
     */
    (void)blocks_call(BLOCK_OP_INFO, 0u, 0u, 0u, -1, &rep);

    return rep.count;
}

/* `count` sectors from `lba` of `unit`, into the region. */
static bool read_sectors(uint32_t unit, uint64_t lba, uint32_t count)
{
    struct block_reply rep;

    if (region == NULL || count == 0u || count > REGION_SECTORS) {
        return false;
    }

    return blocks_call(BLOCK_OP_READ, unit, lba, count, -1, &rep);
}

/*
 * The partitions on one drive, from whichever table it has.
 *
 * GPT first, because a GPT drive also carries an MBR - the protective one -
 * and reading that as the real table would offer the whole drive as a single
 * volume beside its actual partitions. `drives_decode.c` refuses that, and
 * this is the other half of the same care.
 */
static unsigned partitions_of(uint32_t unit, uint64_t drive_blocks,
                              struct drives_part *out, unsigned most)
{
    unsigned found = 0;

    if (!read_sectors(unit, 0u, 1u)) {
        return 0;
    }

    if (mbr_is_protective(region, SECTOR)) {
        uint64_t at = 0;
        unsigned each = 0, count = 0, need;

        if (!read_sectors(unit, 1u, 1u)
            || !gpt_header_at(region, SECTOR, 1u)
            || !gpt_entry_array(region, SECTOR, BLOCK_TRANSFER_MOST,
                                &at, &each, &count)) {
            return 0;
        }

        need = (unsigned)(((uint64_t)each * count + SECTOR - 1u) / SECTOR);

        if (need == 0u || need > REGION_SECTORS || !read_sectors(unit, at, need)) {
            return 0;
        }

        found = gpt_partitions(region, need * SECTOR, each, count, out, most);
    } else {
        found = mbr_partitions(region, SECTOR, out, most);
    }

    /*
     * A partition the drive is not big enough to hold is not one.
     *
     * `fat_decode.h` draws this line deliberately - "whether `total_sectors`
     * fits the partition it was found in is the caller's to check, since only
     * the caller knows the partition" - and the same applies one level up:
     * only here is the drive's size known.
     */
    {
        unsigned i, kept = 0;

        for (i = 0; i < found; i++) {
            if (out[i].first < drive_blocks
                && out[i].sectors <= drive_blocks - out[i].first) {
                out[kept++] = out[i];
            }
        }

        found = kept;
    }

    return found;
}

/* How many clusters a FAT volume has free, counted out of its table. FAT16
 * only: FAT32's table is megabytes and its hint is used instead. */
static bool fat16_free(struct volume *v, uint32_t *out)
{
    uint32_t counted = 0, cluster = 2u;
    uint32_t last = v->fat.clusters + 1u;

    while (cluster <= last) {
        uint32_t sector, offset, run, i;

        if (!fat_entry_place(&v->fat, cluster, &sector, &offset)) {
            return false;
        }

        /* Whole sectors at a time, from wherever this cluster's entry is. */
        run = REGION_SECTORS;

        if (!read_sectors(v->unit, v->first + sector, run)) {
            run = 1u;

            if (!read_sectors(v->unit, v->first + sector, run)) {
                return false;
            }
        }

        for (i = offset; i + 1u < run * SECTOR && cluster <= last; i += 2u) {
            uint32_t next = 0;

            if (fat_link_at(&v->fat, region + i, &next) == FAT_LINK_FREE) {
                counted++;
            }

            cluster++;
        }
    }

    *out = counted;
    return true;
}

/* What a volume's free space is, by whichever route its filesystem allows. */
static void measure_free(struct volume *v)
{
    v->free_bytes = 0;
    v->free_exact = false;

    if (v->fs == FS_KIND_KFS) {
        struct kfs_super sb;
        const char *why = NULL;
        uint32_t free = 0, block;

        /* kfs's block is 4096 and a sector is 512, so block N is sector 8N. */
        if (!read_sectors(v->unit, v->first, KFS_BLOCK / SECTOR)
            || !kfs_super_from(region, KFS_BLOCK, &sb, &why)) {
            return;
        }

        for (block = 0; block < sb.bitmap_blocks; block++) {
            if (!read_sectors(v->unit,
                              v->first + (uint64_t)(sb.bitmap_at + block)
                              * (KFS_BLOCK / SECTOR),
                              KFS_BLOCK / SECTOR)) {
                return;
            }

            free = kfs_free_in(region, KFS_BLOCK, free, sb.blocks);
        }

        v->free_bytes = (uint64_t)free * KFS_BLOCK;
        v->free_exact = true;
        return;
    }

    if (v->fs == FS_KIND_FAT32) {
        struct fat_fsinfo info;
        uint32_t where;

        if (!read_sectors(v->unit, v->first, 1u)) {
            return;
        }

        where = fat_fsinfo_sector(region, SECTOR, &v->fat);

        if (where != 0u && read_sectors(v->unit, v->first + where, 1u)
            && fat_fsinfo_from(region, SECTOR, &info) && info.free_known
            && info.free_clusters <= v->fat.clusters) {
            v->free_bytes = (uint64_t)info.free_clusters * v->fat.cluster_bytes;
            v->free_exact = false;      /* a hint, and it says so */
        }

        return;
    }

    if (v->fs == FS_KIND_FAT16) {
        uint32_t free = 0;

        if (fat16_free(v, &free)) {
            v->free_bytes = (uint64_t)free * v->fat.cluster_bytes;
            v->free_exact = true;
        }
    }
}

/* What filesystem a partition holds, and its label if it has one. */
static bool identify(struct volume *v)
{
    struct fat_volume fat;
    struct kfs_super sb;
    const char *why = NULL;

    if (!read_sectors(v->unit, v->first, 1u)) {
        return false;
    }

    if (fat_volume_from(region, SECTOR, &fat, &why)) {
        if (fat.kind == FAT_16 || fat.kind == FAT_32) {
            v->fat = fat;
            v->fs = (fat.kind == FAT_16) ? FS_KIND_FAT16 : FS_KIND_FAT32;
            v->readable = true;
            drives_label_name(v->name, sizeof(v->name), fat.label);
            return true;
        }

        /* FAT12, which `fat_decode.c` names and refuses. */
        v->fs = FS_KIND_OTHER;
        v->readable = false;
        drives_label_name(v->name, sizeof(v->name), "");
        return true;
    }

    /* kfs keeps its superblock in block 0, which is eight sectors. */
    if (read_sectors(v->unit, v->first, KFS_BLOCK / SECTOR)
        && kfs_super_from(region, KFS_BLOCK, &sb, &why)) {
        v->fs = FS_KIND_KFS;

        /*
         * **Listed, and not yet opened.** kfs's reader is `user/lib/kfs.lua`
         * and this server is C, so its contents come through `/home` where
         * they already are. Reading one here means kfs in C, which is its own
         * piece of work and is on the roadmap behind Disk Benchmark.
         */
        v->readable = false;
        drives_label_name(v->name, sizeof(v->name), "");
        return true;
    }

    /* Something is there and this cannot read it: NTFS, ext4, exFAT until
     * 6f. Shown rather than hidden - a drive with a partition missing looks
     * broken, and "unknown" is a fact about this system rather than the
     * drive. */
    v->fs = FS_KIND_OTHER;
    v->readable = false;
    drives_label_name(v->name, sizeof(v->name), "");

    return true;
}

/* Every volume on every drive, named. */
static void scan(void)
{
    unsigned u, total = units();

    volume_count = 0;

    for (u = 0; u < total && volume_count < VOLUMES_MOST; u++) {
        struct block_reply info;
        struct drives_part parts[DRIVES_PARTS_MAX];
        unsigned n, i;

        if (!blocks_call(BLOCK_OP_INFO, u, 0u, 0u, -1, &info)) {
            continue;           /* a unit that has left */
        }

        if (info.block_size != SECTOR) {
            continue;
        }

        n = partitions_of(u, info.blocks, parts, DRIVES_PARTS_MAX);

        /*
         * A drive with no partition table at all can still be a volume -
         * mtools makes them and `tools/test_fat.py` tests them, and a stick
         * formatted by a camera often is one.
         */
        if (n == 0u) {
            parts[0].first = 0u;
            parts[0].sectors = info.blocks;
            parts[0].type = 0u;
            parts[0].gpt = false;
            n = 1u;
        }

        for (i = 0; i < n && volume_count < VOLUMES_MOST; i++) {
            struct volume *v = &volumes[volume_count];
            const char *taken[VOLUMES_MOST];
            unsigned k;

            zero(v, sizeof(*v));
            v->unit = u;
            v->partition = i;
            v->first = parts[i].first;
            v->sectors = parts[i].sectors;
            v->bytes = parts[i].sectors * SECTOR;

            if (!identify(v)) {
                continue;
            }

            /* Numbered against what is already listed, in arrival order -
             * Diego's answer of 16 September. */
            for (k = 0; k < volume_count; k++) {
                taken[k] = volumes[k].name;
            }

            drives_unique_name(v->name, sizeof(v->name), taken, volume_count);
            measure_free(v);
            volume_count++;
        }
    }

    scanned_at = kosmos_ticks();
}

/* Scanned again when the answer is old enough to be worth doubting. */
static void fresh(void)
{
    unsigned long now = kosmos_ticks();

    if (scanned_at == 0u || now - scanned_at > RESCAN_TICKS) {
        scan();
    }
}

static void reply_with(uint64_t to, const struct drives_reply *rep)
{
    struct message out;

    zero(&out, sizeof(out));
    out.length = sizeof(*rep);
    copy(out.data, rep, sizeof(*rep));

    (void)kosmos_reply(to, &out);
}

static void fail(uint64_t to, uint32_t code)
{
    struct drives_reply rep;

    zero(&rep, sizeof(rep));
    rep.error = code;
    reply_with(to, &rep);
}

/*
 * The volume a path names, and what is left of the path after it.
 *
 * `/PHOTOS 2024/Italy/x.jpg` is the volume `PHOTOS 2024` and `Italy/x.jpg`.
 *
 * **`named` is what tells the two kinds of NULL apart**, and leaving it out
 * was a real bug: `/drives/nonesuch` also ends with `rest` empty, so a path
 * naming a volume that is not there was answered with the list of volumes,
 * as though the caller had asked for `/drives`. `n` is the difference and
 * this function is the only place that has it - zero means the root was
 * asked for, anything else means a name was given and did not match.
 */
static struct volume *volume_for(const char *path, const char **rest,
                                 bool *named)
{
    unsigned i, n = 0;
    const char *at = path;

    while (*at == '/') {
        at++;
    }

    while (at[n] != '\0' && at[n] != '/') {
        n++;
    }

    *rest = at + n;

    while (**rest == '/') {
        (*rest)++;
    }

    if (named != NULL) {
        *named = n > 0u;
    }

    if (n == 0u) {
        return NULL;
    }

    for (i = 0; i < volume_count; i++) {
        if (len_of(volumes[i].name) == n) {
            unsigned k;
            bool same = true;

            for (k = 0; k < n; k++) {
                if (volumes[i].name[k] != at[k]) {
                    same = false;
                    break;
                }
            }

            if (same) {
                return &volumes[i];
            }
        }
    }

    return NULL;
}

/* The volumes themselves, a page at a time. */
static void answer_volumes(uint64_t sender, const struct drives_request *req)
{
    struct drives_reply rep;
    unsigned at = req->offset, i;

    zero(&rep, sizeof(rep));

    for (i = 0; i < DRIVES_VOLUMES_MAX && at + i < volume_count; i++) {
        const struct volume *v = &volumes[at + i];
        struct drives_volume *out = &rep.u.volumes[i];

        copy(out->name, v->name, sizeof(out->name));
        out->fs = (uint32_t)v->fs;
        out->free_exact = v->free_exact ? 1u : 0u;
        out->bytes = v->bytes;
        out->free_bytes = v->free_bytes;
        out->unit = v->unit;
        out->partition = v->partition;
        out->readable = v->readable ? 1u : 0u;
    }

    rep.count = i;
    rep.more = (at + i < volume_count) ? 1u : 0u;
    rep.directory = 1u;

    reply_with(sender, &rep);
}

static void answer(const struct message *msg, uint64_t sender)
{
    struct drives_request req;
    struct drives_reply rep;
    const char *rest = NULL;
    bool named = false;
    struct volume *v;

    if (msg->length < sizeof(req)) {
        fail(sender, DRIVES_ERR_BAD_OP);
        return;
    }

    copy(&req, msg->data, sizeof(req));

    /* Whatever arrived, terminated: 256 bytes from another process, and
     * nothing promises there is a zero in them. */
    req.path[DRIVES_PATH_MAX - 1u] = '\0';

    if (req.length > DRIVES_DATA_MAX) {
        req.length = DRIVES_DATA_MAX;
    }

    fresh();

    if (req.op == DRIVES_OP_VOLUMES) {
        answer_volumes(sender, &req);
        return;
    }

    if (req.op != DRIVES_OP_LIST && req.op != DRIVES_OP_READ
        && req.op != DRIVES_OP_GETATTR) {
        fail(sender, DRIVES_ERR_BAD_OP);
        return;
    }

    v = volume_for(req.path, &rest, &named);

    /* The root of `/drives` is the volumes, listed as a directory is - but
     * only when no volume was named. A name that matched nothing is a path
     * that is not there. */
    if (v == NULL) {
        if (!named && req.op != DRIVES_OP_READ) {
            if (req.op == DRIVES_OP_GETATTR) {
                zero(&rep, sizeof(rep));
                rep.directory = 1u;
                reply_with(sender, &rep);
                return;
            }

            answer_volumes(sender, &req);
            return;
        }

        fail(sender, DRIVES_ERR_NO_PATH);
        return;
    }

    if (!v->readable) {
        /* A volume this server can name and cannot open. The error says
         * which, so a person is told "kfs, and its reader is elsewhere"
         * rather than "no such path". */
        fail(sender, DRIVES_ERR_UNREADABLE);
        return;
    }

    /* A volume with nothing after it is a directory: its root. */
    if (*rest == '\0' && req.op == DRIVES_OP_GETATTR) {
        zero(&rep, sizeof(rep));
        rep.directory = 1u;
        reply_with(sender, &rep);
        return;
    }

    /* FAT directories and files: the next piece. Until it is here, a volume
     * is named and its contents are not offered - which is a different thing
     * from a volume that is not there, and says so. */
    fail(sender, DRIVES_ERR_UNREADABLE);
}

void drives_server(long endpoint, long blocks_cap, long console_cap)
{
    long at;

    blocks = blocks_cap;
    console = console_cap;

    /*
     * The region the driver copies sectors into, made once.
     *
     * Made here rather than per read because `BLOCK_OP_OPEN` is what gives
     * the driver a place to write, and a region per read would be a capability
     * created, sent, mapped and dropped on every partition table - which is
     * the shape of the bug that emptied the capability table on the sixteenth
     * read (`state.md`).
     */
    region_cap = kosmos_mem_create(REGION_PAGES);

    if (region_cap < 0) {
        say(console, "drives: no memory for its buffer\n");
        return;
    }

    at = kosmos_mem_map(region_cap);

    if (at < 0) {
        say(console, "drives: its buffer would not map\n");
        return;
    }

    region = (uint8_t *)(uintptr_t)at;

    {
        struct block_reply rep;

        if (!blocks_call(BLOCK_OP_OPEN, 0u, 0u, 0u, region_cap, &rep)) {
            /*
             * A machine with no USB driver is a supported way to run, and is
             * how every display test runs. The server stays: it answers with
             * no volumes, which is what an empty `/drives` should say, rather
             * than failing to start and taking the namespace's mount with it.
             */
            say(console, "drives: the USB driver would not take its buffer; "
                         "no drives will be listed\n");
        } else {
            handle = rep.handle;
        }
    }

    /*
     * **It says it is here**, once, before it serves.
     *
     * Three probes could not tell "the server answered with nothing" from
     * "no server was reached and the namespace made an empty answer up" -
     * `ns.list` returns the mount points below a path when nothing answers,
     * and returns them *successfully*. One line at startup settles that from
     * outside, the way every other server here says what it is.
     */
    say(console, "drives: serving /drives\n");

    for (;;) {
        struct message msg;
        uint64_t sender = 0;

        if (kosmos_receive(endpoint, &msg, &sender, 0, 0) != 0) {
            return;
        }

        answer(&msg, sender);
    }
}
