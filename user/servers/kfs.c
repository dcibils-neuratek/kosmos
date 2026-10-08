/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The on-disk filesystem in C (`kfs.h`, `docs/diskfs.md` step 1).
 *
 * Laid out as `user/lib/kfs.lua` was, part for part, so the two could be
 * read side by side: the superblock, inodes, talking to the disk, the
 * transaction and the journal, the bitmap, the inode table, file contents,
 * directories, paths, attributes, the operations, formatting. Where this
 * does something the Lua did not, it says so and why; where it says the Lua
 * has the reasoning, that is `git show 48ebe67:user/lib/kfs.lua`, the
 * last revision that had it (`kfs.h`).
 */

#include "kfs.h"

#include <string.h>

/* `user/include/bytes.h`, by its path from here: the host's builds of this
 * file - `build/host/lua`'s and the drive server's test - name no include
 * path for it. */
#include "../include/bytes.h"

const char *kfs_why(int status)
{
    switch (status) {
    case KFS_OK:            return "done";
    case KFS_E_DISK:        return "the disk refused";
    case KFS_E_NOT_KFS:     return "not a kosmos filesystem";
    case KFS_E_VERSION:     return "a version of the format this does not understand";
    case KFS_E_BLOCK_SIZE:  return "blocks of a size this does not understand";
    case KFS_E_LAYOUT:      return "the superblock's layout does not make sense";
    case KFS_E_TOO_SMALL:   return "the disk is too small to hold a filesystem";
    case KFS_E_NO_FILE:     return "no such file";
    case KFS_E_NOT_DIR:     return "not a directory";
    case KFS_E_IS_DIR:      return "that is a directory";
    case KFS_E_TAKEN:       return "that name is taken";
    case KFS_E_NOT_EMPTY:   return "the directory is not empty";
    case KFS_E_FULL:        return "the disk is full";
    case KFS_E_NO_INODES:   return "no inodes left";
    case KFS_E_FRAGMENTED:  return "the file is in more pieces than one write keeps";
    case KFS_E_OPEN:        return "a transaction is already open";
    case KFS_E_NOT_OPEN:    return "no transaction is open";
    case KFS_E_TOO_BIG:     return "more blocks changed than the journal can hold";
    case KFS_E_DOTS:        return "a path may not contain . or ..";
    case KFS_E_ROOT:        return "the root has no parent";
    case KFS_E_NAME_LONG:   return "a name longer than 255 bytes";
    case KFS_E_NAME_EMPTY:  return "a name cannot be empty";
    case KFS_E_INTO_ITSELF: return "a directory cannot be moved into itself";
    case KFS_E_NO_INODE:    return "no such inode";
    case KFS_E_BAD_INODE:   return "an inode claims more extents than fit in one";
    case KFS_E_BAD_DIR:     return "a directory entry is malformed";
    case KFS_E_ATTRS_BIG:   return "more attributes than fit in a block";
    case KFS_E_NOT_ATTRS:   return "this is not an attribute block";
    case KFS_E_DIR_BIG:     return "a directory larger than this can edit";
    case KFS_E_BLOCK_LONG:  return "a block write longer than a block";
    default:                return "an error this does not know";
    }
}

/* Little-endian, as everything on the disk is: `bytes.h`'s `get_le32`,
 * `get_le64`, `put_le32` and `put_le64`, which this file had copies of. */
static uint64_t min64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

void kfs_init(struct kfs *k, const struct kfs_disk *disk)
{
    k->disk = *disk;
    k->open = false;
    k->too_big = false;
    k->held = 0;
    k->freed_runs = 0;
    k->dir_len = 0;
}

/*
 * ------------------------------------------------------------------------
 * The superblock: ten 32-bit fields and the time.
 * ------------------------------------------------------------------------
 */

static void pack_super(uint8_t *block, const struct kfs_super *sb)
{
    memset(block, 0, KFS_BLOCK);
    put_le32(block + 0, sb->magic);
    put_le32(block + 4, sb->version);
    put_le32(block + 8, sb->block_size);
    put_le32(block + 12, sb->blocks);
    put_le32(block + 16, sb->bitmap_at);
    put_le32(block + 20, sb->bitmap_blocks);
    put_le32(block + 24, sb->inodes_at);
    put_le32(block + 28, sb->inode_count);
    put_le32(block + 32, sb->journal_at);
    put_le32(block + 36, sb->data_at);
    put_le64(block + 40, sb->created);
}

/*
 * Held to what a superblock must be before any of its numbers is used as an
 * offset. `kfs.lua` checks the order of the four regions; this also checks
 * that each is as large as the others' places say, which `mkfs` always
 * makes true and a damaged block need not - and here a wrong number would
 * be a read or a write somewhere nobody meant.
 */
static int unpack_super(const uint8_t *block, struct kfs_super *sb)
{
    uint64_t inode_blocks;

    sb->magic = get_le32(block + 0);
    sb->version = get_le32(block + 4);
    sb->block_size = get_le32(block + 8);
    sb->blocks = get_le32(block + 12);
    sb->bitmap_at = get_le32(block + 16);
    sb->bitmap_blocks = get_le32(block + 20);
    sb->inodes_at = get_le32(block + 24);
    sb->inode_count = get_le32(block + 28);
    sb->journal_at = get_le32(block + 32);
    sb->data_at = get_le32(block + 36);
    sb->created = get_le64(block + 40);

    if (sb->magic != KFS_MAGIC) {
        return KFS_E_NOT_KFS;
    }

    if (sb->version != KFS_VERSION) {
        return KFS_E_VERSION;
    }

    if (sb->block_size != KFS_BLOCK) {
        return KFS_E_BLOCK_SIZE;
    }

    if (sb->data_at <= sb->inodes_at || sb->inodes_at <= sb->bitmap_at
        || sb->bitmap_at == 0 || sb->data_at >= sb->blocks) {
        return KFS_E_LAYOUT;
    }

    inode_blocks = ((uint64_t)sb->inode_count * KFS_INODE_SIZE + KFS_BLOCK - 1)
                   / KFS_BLOCK;

    if ((uint64_t)sb->bitmap_blocks * KFS_BLOCK * 8 < sb->blocks
        || (uint64_t)sb->bitmap_at + sb->bitmap_blocks > sb->inodes_at
        || (uint64_t)sb->inodes_at + inode_blocks > sb->journal_at
        || (uint64_t)sb->journal_at + KFS_JOURNAL_BLOCKS > sb->data_at) {
        return KFS_E_LAYOUT;
    }

    return KFS_OK;
}

/*
 * ------------------------------------------------------------------------
 * Inodes. `kfs.lua`'s INODE_HEAD - kind, links, size, mtime, attrs, and how
 * many extents - then the extents, and zeroes to 128 bytes.
 * ------------------------------------------------------------------------
 */

static void pack_inode(uint8_t *p, const struct kfs_inode *node)
{
    memset(p, 0, KFS_INODE_SIZE);
    put_le32(p + 0, node->kind);
    put_le32(p + 4, node->links);
    put_le64(p + 8, node->size);
    put_le64(p + 16, node->mtime);
    put_le32(p + 24, node->attrs);
    put_le32(p + 28, node->extents);

    for (uint32_t i = 0; i < node->extents && i < KFS_EXTENTS; i++) {
        put_le32(p + 32 + i * 8, node->extent[i].start);
        put_le32(p + 36 + i * 8, node->extent[i].count);
    }
}

static int unpack_inode(const uint8_t *p, struct kfs_inode *node)
{
    node->kind = get_le32(p + 0);
    node->links = get_le32(p + 4);
    node->size = get_le64(p + 8);
    node->mtime = get_le64(p + 16);
    node->attrs = get_le32(p + 24);
    node->extents = get_le32(p + 28);

    for (uint32_t i = 0; i < node->extents && i < KFS_EXTENTS; i++) {
        node->extent[i].start = get_le32(p + 32 + i * 8);
        node->extent[i].count = get_le32(p + 36 + i * 8);
    }

    /* More than twelve: the twelfth is the chain's first block, `count` 0
     * (`KFS_EXT_PER_BLOCK`); anything else claiming more is damaged. */
    if (node->extents > KFS_EXTENTS
        && (node->extent[KFS_EXTENTS - 1].count != 0
            || node->extent[KFS_EXTENTS - 1].start == 0)) {
        return KFS_E_BAD_INODE;
    }

    return KFS_OK;
}

/*
 * ------------------------------------------------------------------------
 * Talking to the disk, in runs of as many blocks as one call moves.
 * ------------------------------------------------------------------------
 */

static uint32_t per_call(const struct kfs *k)
{
    return k->disk.most != 0 ? k->disk.most : 1;
}

static int disk_read(struct kfs *k, uint32_t block, uint32_t count, void *to)
{
    uint8_t *at = to;

    while (count > 0) {
        uint32_t n = count < per_call(k) ? count : per_call(k);

        if (k->disk.read(k->disk.ctx, block, n, at) < 0) {
            return KFS_E_DISK;
        }

        block += n;
        count -= n;
        at += (size_t)n * KFS_BLOCK;
    }

    return KFS_OK;
}

static int disk_write(struct kfs *k, uint32_t block, uint32_t count,
                      const void *from)
{
    const uint8_t *at = from;

    while (count > 0) {
        uint32_t n = count < per_call(k) ? count : per_call(k);

        if (k->disk.write(k->disk.ctx, block, n, at) < 0) {
            return KFS_E_DISK;
        }

        block += n;
        count -= n;
        at += (size_t)n * KFS_BLOCK;
    }

    return KFS_OK;
}

/*
 * ------------------------------------------------------------------------
 * The transaction in progress.
 *
 * Between `kfs_begin` and `kfs_commit` a write is held rather than made, and
 * a read answers out of what is held, because the code doing the work reads
 * back what it just wrote. `kfs.lua` has the rest: why a block freed here is
 * not handed out again, and why whole blocks rather than a diff.
 * ------------------------------------------------------------------------
 */

static int held_slot(const struct kfs *k, uint32_t block)
{
    if (!k->open) {
        return -1;
    }

    for (uint32_t i = 0; i < k->held; i++) {
        if (k->held_at[i] == block) {
            return (int)i;
        }
    }

    return -1;
}

int kfs_read_block(struct kfs *k, uint32_t n, void *to)
{
    int slot = held_slot(k, n);

    if (slot >= 0) {
        memcpy(to, k->journal[slot + 1], KFS_BLOCK);
        return KFS_OK;
    }

    return disk_read(k, n, 1, to);
}

/* `count` blocks from `first`: in runs, unless the transaction holds one. */
static int read_blocks(struct kfs *k, uint32_t first, uint32_t count, uint8_t *to)
{
    if (k->open && k->held > 0) {
        for (uint32_t i = 0; i < count; i++) {
            if (held_slot(k, first + i) >= 0) {
                for (uint32_t j = 0; j < count; j++) {
                    int r = kfs_read_block(k, first + j, to + (size_t)j * KFS_BLOCK);

                    if (r != KFS_OK) {
                        return r;
                    }
                }

                return KFS_OK;
            }
        }
    }

    return disk_read(k, first, count, to);
}

/*
 * A whole block, held if a transaction is open and written if not.
 *
 * `kfs.lua` records past the journal's size and refuses at the commit; this
 * refuses at the write that would not fit, since there is nowhere to hold
 * it, and what reads the block back next would otherwise read the old one.
 * The operation fails a step sooner, the same way.
 */
static int put_block(struct kfs *k, uint32_t n, const uint8_t *block)
{
    int slot;

    if (!k->open) {
        return disk_write(k, n, 1, block);
    }

    slot = held_slot(k, n);

    if (slot < 0) {
        if (k->held == KFS_TXN_MAX) {
            k->too_big = true;
            return KFS_E_TOO_BIG;
        }

        slot = (int)k->held++;
        k->held_at[slot] = n;
    }

    if (k->journal[slot + 1] != block) {
        memcpy(k->journal[slot + 1], block, KFS_BLOCK);
    }

    return KFS_OK;
}

int kfs_write_block(struct kfs *k, uint32_t n, const void *from, uint32_t len)
{
    if (len > KFS_BLOCK) {
        return KFS_E_BLOCK_LONG;
    }

    if (from != k->part) {
        memmove(k->part, from, len);
    }

    memset(k->part + len, 0, KFS_BLOCK - len);
    return put_block(k, n, k->part);
}

static void txn_reset(struct kfs *k)
{
    k->open = false;
    k->too_big = false;
    k->held = 0;
    k->freed_runs = 0;
}

int kfs_begin(struct kfs *k)
{
    if (k->open) {
        return KFS_E_OPEN;
    }

    txn_reset(k);
    k->open = true;
    return KFS_OK;
}

void kfs_rollback(struct kfs *k)
{
    txn_reset(k);
}

/*
 * ------------------------------------------------------------------------
 * The journal. `kfs.lua` says what it is for and why each step is where it
 * is; this is the same four steps. At `journal_at` the header, after it the
 * descriptor - where each block belongs, four bytes a block - and after that
 * the blocks.
 * ------------------------------------------------------------------------
 */

static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }

    return h;
}

static void pack_header(uint8_t *block, uint32_t state, uint32_t count,
                        uint32_t sum)
{
    memset(block, 0, KFS_BLOCK);
    put_le32(block + 0, KFS_J_MAGIC);
    put_le32(block + 4, state);
    put_le32(block + 8, count);
    put_le32(block + 12, sum);
    put_le64(block + 16, 0);
}

int kfs_commit(struct kfs *k, const struct kfs_super *sb, bool stop_after_commit)
{
    uint32_t count = k->held;
    uint32_t sum;
    int r;

    if (!k->open) {
        return KFS_E_NOT_OPEN;
    }

    if (count == 0) {
        txn_reset(k);
        return KFS_OK;
    }

    if (k->too_big) {
        txn_reset(k);
        return KFS_E_TOO_BIG;
    }

    /* Closed first, as `kfs.lua` closes it: nothing below is to be held. */
    k->open = false;

    /* 1. The descriptor and the blocks: one run in memory, and on the disk. */
    memset(k->journal[0], 0, KFS_BLOCK);

    for (uint32_t i = 0; i < count; i++) {
        put_le32(k->journal[0] + i * 4, k->held_at[i]);
    }

    sum = fnv1a(0x811c9dc5u, k->journal[0], (size_t)count * 4);

    for (uint32_t i = 0; i < count; i++) {
        sum = fnv1a(sum, k->journal[i + 1], KFS_BLOCK);
    }

    r = disk_write(k, sb->journal_at + 1, count + 1, k->journal[0]);

    if (r != KFS_OK) {
        txn_reset(k);
        return r;
    }

    /* 2. The commit. */
    pack_header(k->head, KFS_J_COMMITTED, count, sum);
    r = disk_write(k, sb->journal_at, 1, k->head);

    if (r != KFS_OK || stop_after_commit) {
        txn_reset(k);
        return r;
    }

    /*
     * 3. Where they belong, sorted so neighbours go in one call. Sorted in
     *    place, which is free now: the order the journal needed is written.
     */
    for (uint32_t i = 0; i + 1 < count; i++) {
        uint32_t least = i;

        for (uint32_t j = i + 1; j < count; j++) {
            if (k->held_at[j] < k->held_at[least]) {
                least = j;
            }
        }

        if (least != i) {
            uint32_t n = k->held_at[i];

            k->held_at[i] = k->held_at[least];
            k->held_at[least] = n;
            memcpy(k->part, k->journal[i + 1], KFS_BLOCK);
            memcpy(k->journal[i + 1], k->journal[least + 1], KFS_BLOCK);
            memcpy(k->journal[least + 1], k->part, KFS_BLOCK);
        }
    }

    for (uint32_t i = 0; i < count; ) {
        uint32_t j = i;

        while (j + 1 < count && k->held_at[j + 1] == k->held_at[j] + 1) {
            j++;
        }

        r = disk_write(k, k->held_at[i], j - i + 1, k->journal[i + 1]);

        if (r != KFS_OK) {
            /* Left committed: the next mount finishes it. */
            txn_reset(k);
            return r;
        }

        i = j + 1;
    }

    /* 4. Done with. */
    pack_header(k->head, KFS_J_EMPTY, 0, 0);
    r = disk_write(k, sb->journal_at, 1, k->head);
    txn_reset(k);
    return r;
}

/*
 * The blocks are read twice over in `kfs.lua` - all of them, then written -
 * and here into the transaction's memory, which is free: recovering is
 * refused while a transaction is open, which nothing does.
 */
int kfs_recover(struct kfs *k, const struct kfs_super *sb, uint32_t *replayed)
{
    uint32_t state, count, sum, check;
    int r;

    *replayed = 0;

    if (k->open) {
        return KFS_E_OPEN;
    }

    r = kfs_read_block(k, sb->journal_at, k->head);

    if (r != KFS_OK) {
        return r;
    }

    state = get_le32(k->head + 4);
    count = get_le32(k->head + 8);
    sum = get_le32(k->head + 12);

    if (get_le32(k->head) != KFS_J_MAGIC || state != KFS_J_COMMITTED) {
        return KFS_OK;
    }

    if (count == 0 || count > KFS_TXN_MAX) {
        return KFS_OK;
    }

    for (uint32_t i = 0; i <= count; i++) {
        r = kfs_read_block(k, sb->journal_at + 1 + i, k->journal[i]);

        if (r != KFS_OK) {
            return r;
        }
    }

    check = fnv1a(0x811c9dc5u, k->journal[0], (size_t)count * 4);

    for (uint32_t i = 1; i <= count; i++) {
        check = fnv1a(check, k->journal[i], KFS_BLOCK);
    }

    /* The checksum decides: a torn commit never happened. */
    if (check != sum) {
        pack_header(k->head, KFS_J_EMPTY, 0, 0);
        return disk_write(k, sb->journal_at, 1, k->head);
    }

    for (uint32_t i = 1; i <= count; i++) {
        r = disk_write(k, get_le32(k->journal[0] + (i - 1) * 4), 1, k->journal[i]);

        if (r != KFS_OK) {
            return r;
        }
    }

    pack_header(k->head, KFS_J_EMPTY, 0, 0);
    r = disk_write(k, sb->journal_at, 1, k->head);

    if (r == KFS_OK) {
        *replayed = count;
    }

    return r;
}

/*
 * ------------------------------------------------------------------------
 * The block bitmap: a bit a block, set when it is used, read and written a
 * block at a time.
 * ------------------------------------------------------------------------
 */

static int bitmap_range(struct kfs *k, const struct kfs_super *sb,
                        uint64_t first, uint64_t count, bool used)
{
    const uint64_t per = (uint64_t)KFS_BLOCK * 8;
    uint64_t done = 0;

    while (done < count) {
        uint64_t block = first + done;
        uint32_t at = sb->bitmap_at + (uint32_t)(block / per);
        uint64_t lo = block % per;
        uint64_t n = min64(count - done, per - lo);
        int r = kfs_read_block(k, at, k->map);

        if (r != KFS_OK) {
            return r;
        }

        for (uint64_t b = lo; b < lo + n; ) {
            if (b % 8 == 0 && lo + n - b >= 8) {
                k->map[b / 8] = used ? 0xff : 0x00;
                b += 8;
            } else {
                uint8_t bit = (uint8_t)(1u << (b % 8));

                k->map[b / 8] = used ? (uint8_t)(k->map[b / 8] | bit)
                                     : (uint8_t)(k->map[b / 8] & ~bit);
                b++;
            }
        }

        r = put_block(k, at, k->map);

        if (r != KFS_OK) {
            return r;
        }

        done += n;
    }

    return KFS_OK;
}

static bool freed_here(const struct kfs *k, uint64_t block)
{
    if (!k->open) {
        return false;
    }

    for (uint32_t i = 0; i < k->freed_runs; i++) {
        if (block >= k->freed[i].start
            && block < (uint64_t)k->freed[i].start + k->freed[i].count) {
            return true;
        }
    }

    return false;
}

static int note_freed(struct kfs *k, uint32_t first, uint32_t count)
{
    /* Joined to a run it continues, which is the usual case: an extent. */
    for (uint32_t i = 0; i < k->freed_runs; i++) {
        struct kfs_extent *run = &k->freed[i];

        if ((uint64_t)run->start + run->count == first) {
            run->count += count;
            return KFS_OK;
        }

        if ((uint64_t)first + count == run->start) {
            run->start = first;
            run->count += count;
            return KFS_OK;
        }
    }

    if (k->freed_runs == KFS_FREED_RUNS) {
        k->too_big = true;
        return KFS_E_TOO_BIG;
    }

    k->freed[k->freed_runs].start = first;
    k->freed[k->freed_runs].count = count;
    k->freed_runs++;
    return KFS_OK;
}

/*
 * Up to `want` free blocks one after another, marked used: the first free
 * block, and as many after it as are free too. First fit, from the start of
 * the bitmap, skipping a whole used byte in one test - `kfs.lua`'s scan,
 * which is what decides where a file goes and so has to be the same.
 */
int kfs_alloc_run(struct kfs *k, const struct kfs_super *sb, uint32_t want,
                  uint32_t *start, uint32_t *got)
{
    const uint64_t per = (uint64_t)KFS_BLOCK * 8;
    bool found = false;
    uint64_t first = 0, count = 0;

    for (uint32_t at = 0; at < sb->bitmap_blocks; at++) {
        int r = kfs_read_block(k, sb->bitmap_at + at, k->map);

        if (r != KFS_OK) {
            return r;
        }

        for (uint32_t byte = 0; byte < KFS_BLOCK; byte++) {
            uint8_t v = k->map[byte];

            if (!found && v == 0xff) {
                continue;
            }

            for (uint32_t bit = 0; bit < 8; bit++) {
                uint64_t block = (uint64_t)at * per + (uint64_t)byte * 8 + bit;
                bool open_bit;

                if (block >= sb->blocks) {
                    goto done;
                }

                open_bit = (v & (1u << bit)) == 0 && !freed_here(k, block);

                if (found) {
                    if (!open_bit) {
                        goto done;
                    }

                    count++;
                } else if (open_bit) {
                    found = true;
                    first = block;
                    count = 1;
                }

                if (found && count >= want) {
                    goto done;
                }
            }
        }
    }

done:
    if (!found) {
        return KFS_E_FULL;
    }

    {
        int r = bitmap_range(k, sb, first, count, true);

        if (r != KFS_OK) {
            return r;
        }
    }

    *start = (uint32_t)first;
    *got = (uint32_t)count;
    return KFS_OK;
}

static int alloc_block(struct kfs *k, const struct kfs_super *sb, uint32_t *block)
{
    uint32_t got;

    return kfs_alloc_run(k, sb, 1, block, &got);
}

/* Given back - and, inside a transaction, kept from being handed out again. */
int kfs_free_run(struct kfs *k, const struct kfs_super *sb, uint32_t first,
                 uint32_t count)
{
    if (count == 0) {
        return KFS_OK;
    }

    if (k->open) {
        int r = note_freed(k, first, count);

        if (r != KFS_OK) {
            return r;
        }
    }

    return bitmap_range(k, sb, first, count, false);
}

/* Counted out of the bitmap, only for blocks the disk has (`kfs.lua`). */
uint64_t kfs_bitmap_free(const uint8_t *map, uint64_t bits)
{
    uint64_t whole = bits / 8, n = 0;

    for (uint64_t byte = 0; byte < whole; byte++) {
        uint8_t v = map[byte];

        for (uint32_t bit = 0; bit < 8; bit++) {
            n += (v >> bit & 1u) == 0;
        }
    }

    for (uint64_t bit = 0; bit < bits % 8; bit++) {
        n += (map[whole] >> bit & 1u) == 0;
    }

    return n;
}

int kfs_free_blocks(struct kfs *k, const struct kfs_super *sb, uint64_t *count)
{
    const uint64_t per = (uint64_t)KFS_BLOCK * 8;
    uint64_t n = 0;

    for (uint32_t at = 0; at < sb->bitmap_blocks; at++) {
        uint64_t bits;
        int r;

        if ((uint64_t)at * per >= sb->blocks) {
            break;
        }

        bits = min64(per, sb->blocks - (uint64_t)at * per);
        r = kfs_read_block(k, sb->bitmap_at + at, k->map);

        if (r != KFS_OK) {
            return r;
        }

        n += kfs_bitmap_free(k->map, bits);
    }

    *count = n;
    return KFS_OK;
}

/*
 * ------------------------------------------------------------------------
 * The inode table.
 * ------------------------------------------------------------------------
 */

#define INODES_A_BLOCK (KFS_BLOCK / KFS_INODE_SIZE)

int kfs_read_inode(struct kfs *k, const struct kfs_super *sb, uint32_t number,
                   struct kfs_inode *out)
{
    int r;

    if (number >= sb->inode_count) {
        return KFS_E_NO_INODE;
    }

    r = kfs_read_block(k, sb->inodes_at + number / INODES_A_BLOCK, k->ino);

    if (r != KFS_OK) {
        return r;
    }

    return unpack_inode(k->ino + (number % INODES_A_BLOCK) * KFS_INODE_SIZE, out);
}

int kfs_write_inode(struct kfs *k, const struct kfs_super *sb, uint32_t number,
                    const struct kfs_inode *node)
{
    uint32_t at;
    int r;

    if (number >= sb->inode_count) {
        return KFS_E_NO_INODE;
    }

    at = sb->inodes_at + number / INODES_A_BLOCK;
    r = kfs_read_block(k, at, k->ino);

    if (r != KFS_OK) {
        return r;
    }

    pack_inode(k->ino + (number % INODES_A_BLOCK) * KFS_INODE_SIZE, node);
    return put_block(k, at, k->ino);
}

/*
 * The first free one from 2: 0 is "none" and 1 the root. `kfs.lua` reads the
 * table's block again for each inode; this reads each block once, and finds
 * the same one.
 */
int kfs_alloc_inode(struct kfs *k, const struct kfs_super *sb, uint32_t *number)
{
    for (uint32_t n = 2; n < sb->inode_count; n++) {
        struct kfs_inode node;

        if (n == 2 || n % INODES_A_BLOCK == 0) {
            int r = kfs_read_block(k, sb->inodes_at + n / INODES_A_BLOCK, k->ino);

            if (r != KFS_OK) {
                return r;
            }
        }

        if (unpack_inode(k->ino + (n % INODES_A_BLOCK) * KFS_INODE_SIZE, &node)
                == KFS_OK
            && node.kind == KFS_KIND_FREE) {
            *number = n;
            return KFS_OK;
        }
    }

    return KFS_E_NO_INODES;
}

/*
 * ------------------------------------------------------------------------
 * File contents.
 * ------------------------------------------------------------------------
 */

/*
 * **Every extent of a file, in order** - the inode's, then its chain's
 * (`KFS_EXT_PER_BLOCK`) - handed to `each`, which stops the walk with
 * anything but `KFS_OK`. With `chain`, each of the chain's own blocks is
 * handed over too, as a run of one, after the extents it held: what freeing
 * a file has to give back.
 */
typedef int (*extent_fn)(struct kfs *k, void *ctx, const struct kfs_extent *x);

static int each_extent(struct kfs *k, const struct kfs_inode *node, bool chain,
                       extent_fn each, void *ctx)
{
    uint32_t here = node->extents > KFS_EXTENTS ? KFS_EXTENTS - 1 : node->extents;
    uint32_t left = node->extents - here;
    uint32_t block = node->extents > KFS_EXTENTS ? node->extent[KFS_EXTENTS - 1].start : 0;

    for (uint32_t e = 0; e < here; e++) {
        int r = each(k, ctx, &node->extent[e]);

        if (r != KFS_OK) return r;
    }

    while (left > 0) {
        uint32_t take = left < KFS_EXT_PER_BLOCK ? left : KFS_EXT_PER_BLOCK;
        uint32_t next;
        int r;

        if (block == 0) return KFS_E_BAD_INODE;

        r = kfs_read_block(k, block, k->ext);

        if (r != KFS_OK) return r;

        next = get_le32(k->ext + KFS_EXT_PER_BLOCK * 8);

        for (uint32_t i = 0; i < take; i++) {
            struct kfs_extent x = { get_le32(k->ext + i * 8), get_le32(k->ext + i * 8 + 4) };

            if (x.count == 0) return KFS_E_BAD_INODE;

            r = each(k, ctx, &x);

            if (r != KFS_OK) return r;

            /* `each` may have read a block of its own; the chain's is read
             * again for the next. */
            if (i + 1 < take) {
                r = kfs_read_block(k, block, k->ext);

                if (r != KFS_OK) return r;
            }
        }

        if (chain) {
            struct kfs_extent own = { block, 1 };

            r = each(k, ctx, &own);

            if (r != KFS_OK) return r;
        }

        left -= take;
        block = next;
    }

    return KFS_OK;
}

struct reading {
    uint64_t offset, finish, start;
    uint8_t *out;
};

static int read_extent(struct kfs *k, void *ctx, const struct kfs_extent *x)
{
    struct reading *rd = ctx;
    uint64_t span = (uint64_t)x->count * KFS_BLOCK;
    uint64_t start = rd->start;
    uint64_t now = rd->offset > start ? rd->offset : start;
    uint64_t end = min64(rd->finish, start + span);

    rd->start += span;

    if (start >= rd->finish) return KFS_OK;

    while (now < end) {
        uint32_t block = (uint32_t)((now - start) / KFS_BLOCK);
        uint64_t skip = now - (start + (uint64_t)block * KFS_BLOCK);
        uint8_t *place = rd->out + (now - rd->offset);
        int r;

        if (skip == 0 && end - now >= KFS_BLOCK) {
            uint32_t whole = (uint32_t)((end - now) / KFS_BLOCK);

            r = read_blocks(k, x->start + block, whole, place);

            if (r != KFS_OK) return r;

            now += (uint64_t)whole * KFS_BLOCK;
        } else {
            uint64_t take = min64(KFS_BLOCK - skip, end - now);

            r = kfs_read_block(k, x->start + block, k->part);

            if (r != KFS_OK) return r;

            memcpy(place, k->part + skip, (size_t)take);
            now += take;
        }
    }

    return KFS_OK;
}

int kfs_read_range(struct kfs *k, const struct kfs_super *sb,
                   const struct kfs_inode *node, uint64_t offset, uint64_t want,
                   void *to, uint64_t *placed)
{
    uint8_t *out = to;
    uint64_t start = 0, finish;

    (void)sb;
    *placed = 0;

    if (offset >= node->size) {
        return KFS_OK;
    }

    if (want > node->size - offset) {
        want = node->size - offset;
    }

    finish = offset + want;

    {
        struct reading rd = { offset, finish, start, out };
        int r = each_extent(k, node, false, read_extent, &rd);

        if (r != KFS_OK) return r;
    }

    *placed = want;
    return KFS_OK;
}

static int free_extent(struct kfs *k, void *ctx, const struct kfs_extent *x)
{
    return kfs_free_run(k, ctx, x->start, x->count);
}

/* Every block a file's contents hold given back - its chain's too. */
static int release(struct kfs *k, const struct kfs_super *sb,
                   struct kfs_inode *node)
{
    int r = each_extent(k, node, true, free_extent, (void *)sb);

    node->extents = 0;
    node->size = 0;
    return r;
}

/* What a write had taken, given back when it cannot finish. */
static void give_back(struct kfs *k, const struct kfs_super *sb,
                      const uint32_t *chain, uint32_t chained)
{
    for (uint32_t i = 0; i < k->runs_n; i++) {
        kfs_free_run(k, sb, k->runs[i].start, k->runs[i].count);
    }

    for (uint32_t i = 0; i < chained; i++) {
        kfs_free_run(k, sb, chain[i], 1);
    }

    k->runs_n = 0;
}

/*
 * Written once, straight to blocks nothing points at yet, and never through
 * the journal (`kfs.lua`'s `write_data`, `design.md` 8.3b): whole blocks
 * from where they are, the last one's part through a block of this one's
 * own, padded.
 */
static int write_data(struct kfs *k, uint32_t first, const uint8_t *bytes,
                      uint64_t len)
{
    uint32_t whole = (uint32_t)(len / KFS_BLOCK);
    uint64_t tail = len % KFS_BLOCK;
    int r = disk_write(k, first, whole, bytes);

    if (r != KFS_OK || tail == 0) {
        return r;
    }

    memcpy(k->part, bytes + (uint64_t)whole * KFS_BLOCK, (size_t)tail);
    memset(k->part + tail, 0, KFS_BLOCK - tail);
    return disk_write(k, first + whole, 1, k->part);
}

int kfs_write_file(struct kfs *k, const struct kfs_super *sb, uint32_t number,
                   struct kfs_inode *node, const void *bytes, uint64_t size)
{
    const uint8_t *from = bytes;
    uint64_t left = (size + KFS_BLOCK - 1) / KFS_BLOCK;
    uint64_t offset = 0;
    uint32_t chain[KFS_FILE_RUNS / KFS_EXT_PER_BLOCK + 2];
    uint32_t chained = 0;
    int r = release(k, sb, node);

    if (r != KFS_OK) {
        return r;
    }

    k->runs_n = 0;

    /*
     * Every run taken and written first, kept in `runs`, so a failure can
     * give back exactly what it took. A new extent for each run: a run ends
     * at the first block that is not free, so the next begins somewhere
     * else and could never be joined to it.
     */
    while (left > 0) {
        uint32_t start, got;
        uint64_t want;

        if (k->runs_n == KFS_FILE_RUNS) {
            give_back(k, sb, chain, chained);
            return KFS_E_FRAGMENTED;
        }

        r = kfs_alloc_run(k, sb, (uint32_t)min64(left, UINT32_MAX), &start, &got);

        if (r != KFS_OK) {
            give_back(k, sb, chain, chained);
            return r;
        }

        k->runs[k->runs_n].start = start;
        k->runs[k->runs_n].count = got;
        k->runs_n++;

        want = min64((uint64_t)got * KFS_BLOCK, size - offset);
        r = write_data(k, start, from + offset, want);

        if (r != KFS_OK) {
            give_back(k, sb, chain, chained);
            return r;
        }

        offset += want;
        left -= got;
    }

    /* Twelve or fewer: in the inode, exactly as a file always was. */
    if (k->runs_n <= KFS_EXTENTS) {
        for (uint32_t i = 0; i < k->runs_n; i++) node->extent[i] = k->runs[i];

        node->extents = k->runs_n;
        node->size = size;
        k->runs_n = 0;
        return kfs_write_inode(k, sb, number, node);
    }

    /*
     * More: eleven in the inode and the rest in a chain of extent blocks
     * (`KFS_EXT_PER_BLOCK`), each taken and written before the inode that
     * points at them - written once, straight to blocks nothing points at
     * yet, as a file's data is.
     */
    {
        uint32_t rest = k->runs_n - (KFS_EXTENTS - 1);
        uint32_t blocks = (rest + KFS_EXT_PER_BLOCK - 1) / KFS_EXT_PER_BLOCK;

        for (uint32_t b = 0; b < blocks; b++) {
            uint32_t got;

            r = kfs_alloc_run(k, sb, 1, &chain[chained], &got);

            if (r != KFS_OK) {
                give_back(k, sb, chain, chained);
                return r;
            }

            chained++;
        }

        for (uint32_t b = 0; b < blocks; b++) {
            uint32_t first = (KFS_EXTENTS - 1) + b * KFS_EXT_PER_BLOCK;
            uint32_t n = k->runs_n - first < KFS_EXT_PER_BLOCK ? k->runs_n - first
                                                               : KFS_EXT_PER_BLOCK;

            memset(k->ext, 0, KFS_BLOCK);

            for (uint32_t i = 0; i < n; i++) {
                put_le32(k->ext + i * 8, k->runs[first + i].start);
                put_le32(k->ext + i * 8 + 4, k->runs[first + i].count);
            }

            put_le32(k->ext + KFS_EXT_PER_BLOCK * 8, b + 1 < blocks ? chain[b + 1] : 0);

            r = disk_write(k, chain[b], 1, k->ext);

            if (r != KFS_OK) {
                give_back(k, sb, chain, chained);
                return r;
            }
        }

        for (uint32_t i = 0; i < KFS_EXTENTS - 1; i++) node->extent[i] = k->runs[i];

        node->extent[KFS_EXTENTS - 1].start = chain[0];
        node->extent[KFS_EXTENTS - 1].count = 0;
        node->extents = k->runs_n;
    }

    node->size = size;
    k->runs_n = 0;
    return kfs_write_inode(k, sb, number, node);
}

/*
 * ------------------------------------------------------------------------
 * Directories: an ordinary file whose contents are entries - an inode
 * number, a length byte, the name - one after another.
 *
 * `kfs.lua` reads one into a list without its empty entries (inode 0),
 * edits the list, and writes the list back. This reads one into `k->dir`
 * without its empty entries, edits it there, and writes it back - which
 * is the same bytes.
 * ------------------------------------------------------------------------
 */

#define ENTRY_HEAD 5u                           /* inode, and the length */
#define ENTRY_MOST (ENTRY_HEAD + KFS_NAME_MAX)

bool kfs_same_name(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) {
        return false;
    }

    for (size_t i = 0; i < alen; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];

        if (x >= 'A' && x <= 'Z') {
            x = (unsigned char)(x - 'A' + 'a');
        }

        if (y >= 'A' && y <= 'Z') {
            y = (unsigned char)(y - 'A' + 'a');
        }

        if (x != y) {
            return false;
        }
    }

    return true;
}

int kfs_open_dir(struct kfs *k, const struct kfs_super *sb,
                 const struct kfs_inode *dir)
{
    uint64_t got;
    uint32_t in = 0, out = 0, len;
    int r;

    /* Room left for the one entry an edit may add. */
    if (dir->size > KFS_DIR_ROOM - ENTRY_MOST) {
        return KFS_E_DIR_BIG;
    }

    k->dir_len = 0;
    r = kfs_read_range(k, sb, dir, 0, dir->size, k->dir, &got);

    if (r != KFS_OK) {
        return r;
    }

    len = (uint32_t)got;

    while (in < len) {
        uint32_t n;

        if (len - in < ENTRY_HEAD) {
            return KFS_E_BAD_DIR;
        }

        n = k->dir[in + 4];

        if (len - in - ENTRY_HEAD < n) {
            return KFS_E_BAD_DIR;
        }

        if (get_le32(k->dir + in) != 0) {
            if (out != in) {
                memmove(k->dir + out, k->dir + in, ENTRY_HEAD + n);
            }

            out += ENTRY_HEAD + n;
        }

        in += ENTRY_HEAD + n;
    }

    k->dir_len = out;
    return KFS_OK;
}

bool kfs_dir_next(const struct kfs *k, uint32_t *pos, uint32_t *inode,
                  const char **name, uint32_t *name_len)
{
    if (*pos >= k->dir_len) {
        return false;
    }

    *inode = get_le32(k->dir + *pos);
    *name_len = k->dir[*pos + 4];
    *name = (const char *)k->dir + *pos + ENTRY_HEAD;
    *pos += ENTRY_HEAD + *name_len;
    return true;
}

/*
 * Where `name` is in the directory held, and what it names: the first
 * entry with it, or with `last`, the last - `kfs.lua`'s `rename` takes the
 * last, everything else the first. They differ only on a disk made before
 * names were found whatever their case, which can hold two.
 */
static bool dir_find(const struct kfs *k, const char *name, size_t len, bool last,
                     uint32_t *at, uint32_t *inode)
{
    uint32_t pos = 0, here, number, n;
    const char *stored;
    bool found = false;

    while (here = pos, kfs_dir_next(k, &pos, &number, &stored, &n)) {
        if (kfs_same_name(stored, n, name, len)) {
            *at = here;
            *inode = number;
            found = true;

            if (!last) {
                break;
            }
        }
    }

    return found;
}

static int dir_append(struct kfs *k, uint32_t inode, const char *name, size_t len)
{
    if (k->dir_len + ENTRY_HEAD + len > KFS_DIR_ROOM) {
        return KFS_E_DIR_BIG;
    }

    put_le32(k->dir + k->dir_len, inode);
    k->dir[k->dir_len + 4] = (uint8_t)len;
    memcpy(k->dir + k->dir_len + ENTRY_HEAD, name, len);
    k->dir_len += (uint32_t)(ENTRY_HEAD + len);
    return KFS_OK;
}

static void dir_remove(struct kfs *k, uint32_t at)
{
    uint32_t n = ENTRY_HEAD + k->dir[at + 4];

    memmove(k->dir + at, k->dir + at + n, k->dir_len - at - n);
    k->dir_len -= n;
}

static int dir_rename_at(struct kfs *k, uint32_t at, const char *name, size_t len)
{
    uint32_t old = k->dir[at + 4];
    uint32_t tail = at + ENTRY_HEAD + old;

    if (k->dir_len - old + len > KFS_DIR_ROOM) {
        return KFS_E_DIR_BIG;
    }

    memmove(k->dir + at + ENTRY_HEAD + len, k->dir + tail, k->dir_len - tail);
    k->dir[at + 4] = (uint8_t)len;
    memcpy(k->dir + at + ENTRY_HEAD, name, len);
    k->dir_len = (uint32_t)(k->dir_len - old + len);
    return KFS_OK;
}

static int dir_save(struct kfs *k, const struct kfs_super *sb, uint32_t number,
                    struct kfs_inode *dir)
{
    return kfs_write_file(k, sb, number, dir, k->dir, k->dir_len);
}

/*
 * ------------------------------------------------------------------------
 * Paths: walked from the root every time, a component at a time, with no
 * `.` or `..` in them (`kfs.lua` has why).
 * ------------------------------------------------------------------------
 */

/* The next component from `*at`, or false at the end. */
static bool next_part(const char *path, size_t len, size_t *at,
                      const char **part, size_t *n)
{
    size_t i = *at;

    while (i < len && path[i] == '/') {
        i++;
    }

    if (i == len) {
        *at = i;
        return false;
    }

    *part = path + i;

    while (i < len && path[i] != '/') {
        i++;
    }

    *n = (size_t)(path + i - *part);
    *at = i;
    return true;
}

static bool is_dots(const char *part, size_t n)
{
    return (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.');
}

/* How many components, refusing `.` and `..`; the last one, if any. */
static int count_parts(const char *path, size_t len, size_t *count,
                       const char **last, size_t *last_len)
{
    size_t at = 0, n;
    const char *part;

    *count = 0;

    while (next_part(path, len, &at, &part, &n)) {
        if (is_dots(part, n)) {
            return KFS_E_DOTS;
        }

        (*count)++;
        *last = part;
        *last_len = n;
    }

    return KFS_OK;
}

static int walk(struct kfs *k, const struct kfs_super *sb, const char *path,
                size_t len, size_t count, uint32_t *number, struct kfs_inode *node)
{
    size_t at = 0, n = 0;
    const char *part = NULL;
    int r;

    *number = KFS_ROOT_INODE;
    r = kfs_read_inode(k, sb, *number, node);

    for (size_t i = 0; r == KFS_OK && i < count; i++) {
        uint32_t where, found;

        next_part(path, len, &at, &part, &n);

        if (node->kind != KFS_KIND_DIR) {
            return KFS_E_NOT_DIR;
        }

        r = kfs_open_dir(k, sb, node);

        if (r != KFS_OK) {
            return r;
        }

        if (!dir_find(k, part, n, false, &where, &found)) {
            return KFS_E_NO_FILE;
        }

        *number = found;
        r = kfs_read_inode(k, sb, found, node);
    }

    return r;
}

int kfs_find(struct kfs *k, const struct kfs_super *sb, const char *path,
             size_t len, uint32_t *number, struct kfs_inode *node)
{
    size_t count, last_len = 0;
    const char *last = NULL;
    int r = count_parts(path, len, &count, &last, &last_len);

    if (r != KFS_OK) {
        return r;
    }

    return walk(k, sb, path, len, count, number, node);
}

int kfs_parent_of(struct kfs *k, const struct kfs_super *sb, const char *path,
                  size_t len, uint32_t *number, struct kfs_inode *node,
                  const char **name, size_t *name_len)
{
    size_t count;
    int r = count_parts(path, len, &count, name, name_len);

    if (r != KFS_OK) {
        return r;
    }

    if (count == 0) {
        return KFS_E_ROOT;
    }

    r = walk(k, sb, path, len, count - 1, number, node);

    if (r != KFS_OK) {
        return r;
    }

    return node->kind == KFS_KIND_DIR ? KFS_OK : KFS_E_NOT_DIR;
}

/*
 * Each part that exists as it is stored, the rest as given; with
 * `keep_last`, the last as given whatever is there. A path that cannot be
 * split is given back as it came. A stored name is the given one's length,
 * so the answer is never longer than the path and a separator.
 */
size_t kfs_spelled(struct kfs *k, const struct kfs_super *sb, const char *path,
                   size_t len, bool keep_last, char *out)
{
    struct kfs_inode node;
    bool have;
    size_t count, last_len = 0, at = 0, n = 0, used = 0;
    const char *last = NULL, *part = NULL;

    if (count_parts(path, len, &count, &last, &last_len) != KFS_OK || count == 0) {
        memcpy(out, path, len);
        return len;
    }

    have = kfs_read_inode(k, sb, KFS_ROOT_INODE, &node) == KFS_OK;

    for (size_t i = 0; i < count; i++) {
        uint32_t where = 0, number = 0;
        bool found = false;

        next_part(path, len, &at, &part, &n);
        out[used++] = '/';

        if (have && node.kind == KFS_KIND_DIR && !(keep_last && i + 1 == count)
            && kfs_open_dir(k, sb, &node) == KFS_OK
            && dir_find(k, part, n, false, &where, &number)) {
            found = true;
            memcpy(out + used, k->dir + where + ENTRY_HEAD, n);
        } else {
            memcpy(out + used, part, n);
        }

        used += n;
        have = found && kfs_read_inode(k, sb, number, &node) == KFS_OK;
    }

    return used;
}

int kfs_list(struct kfs *k, const struct kfs_super *sb, const char *path,
             size_t len)
{
    struct kfs_inode node;
    uint32_t number;
    int r = kfs_find(k, sb, path, len, &number, &node);

    if (r != KFS_OK) {
        return r;
    }

    if (node.kind != KFS_KIND_DIR) {
        return KFS_E_NOT_DIR;
    }

    return kfs_open_dir(k, sb, &node);
}

/*
 * ------------------------------------------------------------------------
 * Attributes: one block, the inode pointing at it, a length and then what
 * `sys.pack` made (`kfs.lua` has the reasons for each).
 * ------------------------------------------------------------------------
 */

int kfs_read_attrs(struct kfs *k, const struct kfs_super *sb,
                   const struct kfs_inode *node, void *to, uint32_t *len)
{
    uint32_t n;
    int r;

    (void)sb;
    *len = 0;

    if (node->attrs == 0) {
        return KFS_OK;
    }

    r = kfs_read_block(k, node->attrs, k->part);

    if (r != KFS_OK) {
        return r;
    }

    n = get_le32(k->part);

    if (n == 0 || n > KFS_BLOCK - 4) {
        return KFS_E_NOT_ATTRS;
    }

    memcpy(to, k->part + 4, n);
    *len = n;
    return KFS_OK;
}

int kfs_write_attrs(struct kfs *k, const struct kfs_super *sb, uint32_t number,
                    struct kfs_inode *node, const void *bytes, uint32_t len)
{
    uint32_t block = node->attrs;
    int r;

    if (len == 0) {
        if (node->attrs == 0) {
            return KFS_OK;
        }

        r = kfs_free_run(k, sb, node->attrs, 1);

        if (r != KFS_OK) {
            return r;
        }

        node->attrs = 0;
        return kfs_write_inode(k, sb, number, node);
    }

    if (len > KFS_BLOCK - 4) {
        return KFS_E_ATTRS_BIG;
    }

    if (block == 0) {
        r = alloc_block(k, sb, &block);

        if (r != KFS_OK) {
            return r;
        }
    }

    /* The block before the inode, for `kfs.lua`'s reason. */
    memset(k->part, 0, KFS_BLOCK);
    put_le32(k->part, len);
    memcpy(k->part + 4, bytes, len);
    r = put_block(k, block, k->part);

    if (r != KFS_OK) {
        if (node->attrs == 0) {
            kfs_free_run(k, sb, block, 1);
        }

        return r;
    }

    if (node->attrs != block) {
        node->attrs = block;
        return kfs_write_inode(k, sb, number, node);
    }

    return KFS_OK;
}

/*
 * ------------------------------------------------------------------------
 * The operations.
 * ------------------------------------------------------------------------
 */

/* A name added to a directory, or the one already there pointed elsewhere. */
static int link(struct kfs *k, const struct kfs_super *sb, uint32_t dir_number,
                struct kfs_inode *dir, const char *name, size_t len, uint32_t inode)
{
    uint32_t at, was;
    int r = kfs_open_dir(k, sb, dir);

    if (r != KFS_OK) {
        return r;
    }

    if (dir_find(k, name, len, false, &at, &was)) {
        put_le32(k->dir + at, inode);
    } else {
        r = dir_append(k, inode, name, len);

        if (r != KFS_OK) {
            return r;
        }
    }

    return dir_save(k, sb, dir_number, dir);
}

/*
 * A name is held to what an entry can say before anything is written.
 * `kfs.lua` finds out at the directory's write, after the inode is.
 */
static int check_name(const char *name, size_t len)
{
    if (len == 0) {
        return KFS_E_NAME_EMPTY;
    }

    if (len > KFS_NAME_MAX) {
        return KFS_E_NAME_LONG;
    }

    return is_dots(name, len) ? KFS_E_DOTS : KFS_OK;
}

int kfs_mkdir(struct kfs *k, const struct kfs_super *sb, const char *path,
              size_t len, uint64_t now)
{
    struct kfs_inode dir, node;
    uint32_t dir_number, number, at, was;
    const char *name;
    size_t name_len;
    int r = kfs_parent_of(k, sb, path, len, &dir_number, &dir, &name, &name_len);

    if (r != KFS_OK) {
        return r;
    }

    r = kfs_open_dir(k, sb, &dir);

    if (r != KFS_OK) {
        return r;
    }

    if (dir_find(k, name, name_len, false, &at, &was)) {
        return KFS_E_TAKEN;
    }

    r = check_name(name, name_len);

    if (r != KFS_OK) {
        return r;
    }

    r = kfs_alloc_inode(k, sb, &number);

    if (r != KFS_OK) {
        return r;
    }

    memset(&node, 0, sizeof node);
    node.kind = KFS_KIND_DIR;
    node.links = 2;
    node.mtime = now == KFS_NO_TIME ? 0 : now;
    r = kfs_write_inode(k, sb, number, &node);

    if (r != KFS_OK) {
        return r;
    }

    return link(k, sb, dir_number, &dir, name, name_len, number);
}

int kfs_store(struct kfs *k, const struct kfs_super *sb, const char *path,
              size_t len, const void *bytes, uint64_t size, uint64_t now,
              uint32_t *number)
{
    struct kfs_inode dir, node;
    uint32_t dir_number, at, existing;
    const char *name;
    size_t name_len;
    int r = kfs_parent_of(k, sb, path, len, &dir_number, &dir, &name, &name_len);

    if (r != KFS_OK) {
        return r;
    }

    r = kfs_open_dir(k, sb, &dir);

    if (r != KFS_OK) {
        return r;
    }

    if (dir_find(k, name, name_len, false, &at, &existing)) {
        r = kfs_read_inode(k, sb, existing, &node);

        if (r != KFS_OK) {
            return r;
        }

        if (node.kind == KFS_KIND_DIR) {
            return KFS_E_IS_DIR;
        }

        if (now != KFS_NO_TIME) {
            node.mtime = now;
        }

        r = kfs_write_file(k, sb, existing, &node, bytes, size);

        if (r == KFS_OK) {
            *number = existing;
        }

        return r;
    }

    /* New: the inode and its contents, then the entry (`kfs.lua` has why). */
    r = check_name(name, name_len);

    if (r != KFS_OK) {
        return r;
    }

    r = kfs_alloc_inode(k, sb, number);

    if (r != KFS_OK) {
        return r;
    }

    memset(&node, 0, sizeof node);
    node.kind = KFS_KIND_FILE;
    node.links = 1;
    node.mtime = now == KFS_NO_TIME ? 0 : now;
    r = kfs_write_file(k, sb, *number, &node, bytes, size);

    if (r != KFS_OK) {
        return r;
    }

    return link(k, sb, dir_number, &dir, name, name_len, *number);
}

/*
 * A directory entry edited and nothing copied (`kfs.lua` has the argument).
 *
 * Two refusals `kfs.lua` does not make, both of which it should: `.` or `..`
 * as a new name, and a directory moved into itself through a path spelled
 * in another case - the check compared the paths as typed, so `/a` to `/A/b`
 * went past it and made a directory its own grandchild.
 */
int kfs_rename(struct kfs *k, const struct kfs_super *sb, const char *path,
               size_t len, const char *to, size_t to_len)
{
    struct kfs_inode dir, to_dir;
    uint32_t dir_number, to_number, at, inode, there, other;
    const char *name, *to_name = to;
    size_t name_len, to_name_len = to_len;
    int r;

    if (to_len == 0) {
        return KFS_E_NAME_EMPTY;
    }

    r = kfs_parent_of(k, sb, path, len, &dir_number, &dir, &name, &name_len);

    if (r != KFS_OK) {
        return r;
    }

    to_number = dir_number;
    to_dir = dir;

    if (memchr(to, '/', to_len) != NULL) {
        if (to_len > len && to[len] == '/' && kfs_same_name(to, len, path, len)) {
            return KFS_E_INTO_ITSELF;
        }

        r = kfs_parent_of(k, sb, to, to_len, &to_number, &to_dir, &to_name,
                          &to_name_len);

        if (r != KFS_OK) {
            return r;
        }
    }

    r = check_name(to_name, to_name_len);

    if (r != KFS_OK) {
        return r;
    }

    r = kfs_open_dir(k, sb, &dir);

    if (r != KFS_OK) {
        return r;
    }

    if (!dir_find(k, name, name_len, true, &at, &inode)) {
        return KFS_E_NO_FILE;
    }

    /* Within one directory: one edit, one write. Its own name in another
     * case is not taken - that is how a spelling is changed. */
    if (to_number == dir_number) {
        uint32_t pos = 0, here, number, n;
        const char *stored;

        while (here = pos, kfs_dir_next(k, &pos, &number, &stored, &n)) {
            if (here != at && kfs_same_name(stored, n, to_name, to_name_len)) {
                return KFS_E_TAKEN;
            }
        }

        r = dir_rename_at(k, at, to_name, to_name_len);

        if (r != KFS_OK) {
            return r;
        }

        return dir_save(k, sb, dir_number, &dir);
    }

    r = kfs_open_dir(k, sb, &to_dir);

    if (r != KFS_OK) {
        return r;
    }

    if (dir_find(k, to_name, to_name_len, false, &there, &other)) {
        return KFS_E_TAKEN;
    }

    r = dir_append(k, inode, to_name, to_name_len);

    if (r == KFS_OK) {
        r = dir_save(k, sb, to_number, &to_dir);
    }

    if (r != KFS_OK) {
        return r;
    }

    /* Read again, as `kfs.lua` reads it again, and the first entry goes. */
    r = kfs_open_dir(k, sb, &dir);

    if (r != KFS_OK) {
        return r;
    }

    if (dir_find(k, name, name_len, false, &at, &inode)) {
        dir_remove(k, at);
    }

    return dir_save(k, sb, dir_number, &dir);
}

/* The entry first, then what it named (`kfs.lua` has why that order). */
int kfs_unlink(struct kfs *k, const struct kfs_super *sb, const char *path,
               size_t len)
{
    struct kfs_inode dir, node;
    uint32_t dir_number, at, victim;
    const char *name;
    size_t name_len;
    int r = kfs_parent_of(k, sb, path, len, &dir_number, &dir, &name, &name_len);

    if (r != KFS_OK) {
        return r;
    }

    r = kfs_open_dir(k, sb, &dir);

    if (r != KFS_OK) {
        return r;
    }

    if (!dir_find(k, name, name_len, false, &at, &victim)) {
        return KFS_E_NO_FILE;
    }

    r = kfs_read_inode(k, sb, victim, &node);

    if (r != KFS_OK) {
        return r;
    }

    /* A directory has to be empty; reading it takes `k->dir`, so the
     * parent is read again after. */
    if (node.kind == KFS_KIND_DIR) {
        r = kfs_open_dir(k, sb, &node);

        if (r != KFS_OK) {
            return r;
        }

        if (k->dir_len > 0) {
            return KFS_E_NOT_EMPTY;
        }

        r = kfs_open_dir(k, sb, &dir);

        if (r != KFS_OK) {
            return r;
        }

        dir_find(k, name, name_len, false, &at, &victim);
    }

    dir_remove(k, at);
    r = dir_save(k, sb, dir_number, &dir);

    if (r != KFS_OK) {
        return r;
    }

    r = release(k, sb, &node);

    if (r != KFS_OK) {
        return r;
    }

    if (node.attrs != 0) {
        r = kfs_free_run(k, sb, node.attrs, 1);

        if (r != KFS_OK) {
            return r;
        }
    }

    memset(&node, 0, sizeof node);
    return kfs_write_inode(k, sb, victim, &node);
}

/*
 * ------------------------------------------------------------------------
 * Formatting: the bitmap with the metadata used, the inode table with the
 * root in it, the superblock last, and then `/Home` (`kfs.lua` has the
 * reasons for the order, and for `/Home`).
 * ------------------------------------------------------------------------
 */

const char *const kfs_layout[] = { "/Home", NULL };

int kfs_mkfs(struct kfs *k, uint64_t sectors, uint64_t now,
             const char *const *layout, struct kfs_super *sb)
{
    uint64_t blocks = sectors / KFS_PER_BLOCK;
    uint32_t inode_blocks;
    int r;

    if (blocks < 64) {
        return KFS_E_TOO_SMALL;
    }

    if (blocks > UINT32_MAX) {
        return KFS_E_LAYOUT;
    }

    memset(sb, 0, sizeof *sb);
    sb->magic = KFS_MAGIC;
    sb->version = KFS_VERSION;
    sb->block_size = KFS_BLOCK;
    sb->blocks = (uint32_t)blocks;
    sb->inode_count = blocks / 16 > 64 ? (uint32_t)(blocks / 16) : 64;
    sb->bitmap_blocks = (uint32_t)((blocks + KFS_BLOCK * 8 - 1) / (KFS_BLOCK * 8));
    inode_blocks = (uint32_t)(((uint64_t)sb->inode_count * KFS_INODE_SIZE
                               + KFS_BLOCK - 1) / KFS_BLOCK);
    sb->bitmap_at = 1;
    sb->inodes_at = sb->bitmap_at + sb->bitmap_blocks;
    sb->journal_at = sb->inodes_at + inode_blocks;
    sb->data_at = sb->journal_at + KFS_JOURNAL_BLOCKS;
    sb->created = now == KFS_NO_TIME ? 0 : now;

    /* Before the first data block, and past the end of the disk, is used. */
    for (uint32_t i = 0; i < sb->bitmap_blocks; i++) {
        uint64_t first = (uint64_t)i * KFS_BLOCK * 8;

        memset(k->map, 0, KFS_BLOCK);

        for (uint32_t bit = 0; bit < KFS_BLOCK * 8; bit++) {
            uint64_t block = first + bit;

            if (block < sb->data_at || block >= sb->blocks) {
                k->map[bit / 8] = (uint8_t)(k->map[bit / 8] | 1u << (bit % 8));
            }
        }

        r = put_block(k, sb->bitmap_at + i, k->map);

        if (r != KFS_OK) {
            return r;
        }
    }

    for (uint32_t i = 0; i < inode_blocks; i++) {
        memset(k->ino, 0, KFS_BLOCK);

        if (i == KFS_ROOT_INODE / INODES_A_BLOCK) {
            struct kfs_inode root;

            memset(&root, 0, sizeof root);
            root.kind = KFS_KIND_DIR;
            root.links = 2;
            root.mtime = sb->created;
            pack_inode(k->ino + (KFS_ROOT_INODE % INODES_A_BLOCK) * KFS_INODE_SIZE,
                       &root);
        }

        r = put_block(k, sb->inodes_at + i, k->ino);

        if (r != KFS_OK) {
            return r;
        }
    }

    pack_super(k->head, sb);
    r = put_block(k, 0, k->head);

    if (r != KFS_OK) {
        return r;
    }

    for (layout = layout != NULL ? layout : kfs_layout; *layout != NULL; layout++) {
        r = kfs_mkdir(k, sb, *layout, strlen(*layout), now == KFS_NO_TIME ? 0 : now);

        if (r != KFS_OK) {
            return r;
        }
    }

    return KFS_OK;
}

int kfs_super_decode(const uint8_t *block, size_t size, struct kfs_super *sb)
{
    /* Ten 32-bit fields and the time: 48 bytes of the block. */
    if (block == NULL || size < 48) {
        return KFS_E_NOT_KFS;
    }

    return unpack_super(block, sb);
}

int kfs_mount(struct kfs *k, struct kfs_super *sb)
{
    int r = kfs_read_block(k, 0, k->head);

    if (r != KFS_OK) {
        return r;
    }

    return unpack_super(k->head, sb);
}
