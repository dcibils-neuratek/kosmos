/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The disk server's cache of small reads in C, on this machine
 * (`user/servers/diskcache.c`, `docs/diskfs.md` step 3).
 *
 * `test_blockcache.lua`'s checks, over a disk of memory that counts every
 * call that reached it: what the cache must never do is answer with
 * anything the disk does not hold; what it is for is answering a small read
 * twice with one call. Two of the Lua's checks do not carry over, and why is
 * the C's own: a region's write and a part of a block are both bytes the
 * Lua never held as a string, and `kfs.c` writes whole blocks it holds - so
 * a write here updates what is kept, and the rule that replaces those two is
 * that a write never adds a block.
 *
 * And `test_kfs.lua` runs its 87 through it with `KFS_IMPL=c KFS_CACHE=1`.
 */

#include "../user/servers/diskcache.h"

#include <stdio.h>
#include <string.h>

#define BLOCKS 256

static uint8_t disk[BLOCKS][KFS_BLOCK];
static int calls;
static int refuse;

static int dev_read(void *ctx, uint32_t block, uint32_t count, void *to)
{
    (void)ctx;
    calls++;
    memcpy(to, disk[block], (size_t)count * KFS_BLOCK);
    return 0;
}

static int dev_write(void *ctx, uint32_t block, uint32_t count, const void *from)
{
    (void)ctx;
    calls++;

    if (refuse) {
        return -1;
    }

    memcpy(disk[block], from, (size_t)count * KFS_BLOCK);
    return 0;
}

static int passed, failed;

static void check(int ok, const char *what)
{
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("  FAIL: %s\n", what);
    }
}

static void fill(uint8_t *b, char c, uint32_t count)
{
    memset(b, c, (size_t)count * KFS_BLOCK);
}

static int holds(const uint8_t *b, char c, uint32_t count)
{
    for (size_t i = 0; i < (size_t)count * KFS_BLOCK; i++) {
        if (b[i] != (uint8_t)c) {
            return 0;
        }
    }

    return 1;
}

static struct diskcache cache;

int main(void)
{
    static uint8_t buf[8 * KFS_BLOCK];
    struct kfs_disk dev = { NULL, dev_read, dev_write, 31 };
    struct kfs_disk d;

    diskcache_init(&cache, &dev, 8, 4);
    d = diskcache_disk(&cache);

    /* A small read, twice: one call. */
    fill(disk[10], 'a', 1);
    calls = 0;
    d.read(d.ctx, 10, 1, buf);
    check(holds(buf, 'a', 1), "a block read is what the disk holds");
    d.read(d.ctx, 10, 1, buf);
    check(holds(buf, 'a', 1) && calls == 1, "a block read twice is one call");

    /* Written through: the disk has it, and the next read is it, uncalled. */
    fill(buf, 'b', 1);
    calls = 0;
    d.write(d.ctx, 10, 1, buf);
    check(holds(disk[10], 'b', 1), "a write reaches the disk");
    memset(buf, 0, KFS_BLOCK);
    d.read(d.ctx, 10, 1, buf);
    check(holds(buf, 'b', 1) && calls == 1,
          "a read after a write is what was written, answered without a call");

    /* A write never adds a block: what is kept is what was read. */
    fill(buf, 'c', 1);
    d.write(d.ctx, 30, 1, buf);
    calls = 0;
    d.read(d.ctx, 30, 1, buf);
    check(calls == 1 && holds(buf, 'c', 1),
          "a block only written is read from the disk, not kept by the write");

    /* A write the disk refused: whatever it holds now, not what was kept. */
    d.read(d.ctx, 10, 1, buf);
    refuse = 1;
    fill(buf, 'd', 1);
    d.write(d.ctx, 10, 1, buf);
    refuse = 0;
    fill(disk[10], 'e', 1);                /* what a half-done write left */
    calls = 0;
    d.read(d.ctx, 10, 1, buf);
    check(holds(buf, 'e', 1) && calls == 1,
          "a block a refused write covered is not answered from the cache");

    /* Two blocks in one read, kept as two: either is answered alone. */
    fill(disk[20], 'h', 1);
    fill(disk[21], 'i', 1);
    d.read(d.ctx, 20, 2, buf);
    calls = 0;
    d.read(d.ctx, 21, 1, buf);
    check(holds(buf, 'i', 1), "the second of two blocks read together is kept");
    d.read(d.ctx, 20, 2, buf);
    check(holds(buf, 'h', 1) && holds(buf + KFS_BLOCK, 'i', 1) && calls == 0,
          "a two-block read is kept as its two blocks");

    /* A large read is not kept: a file read in one piece. */
    calls = 0;
    d.read(d.ctx, 50, 5, buf);
    d.read(d.ctx, 50, 1, buf);
    check(calls == 2, "a read of more than `small` blocks is not kept");

    /* Bounded, and the least recently used goes first. */
    diskcache_clear(&cache);

    for (uint32_t b = 100; b <= 108; b++) {
        fill(disk[b], (char)('p' + (b - 100)), 1);
    }

    for (uint32_t b = 100; b <= 107; b++) {
        d.read(d.ctx, b, 1, buf);
    }

    d.read(d.ctx, 100, 1, buf);            /* 100 is now the most recent */
    d.read(d.ctx, 108, 1, buf);            /* one past the bound: 101 goes */
    calls = 0;
    d.read(d.ctx, 100, 1, buf);
    check(calls == 0 && holds(buf, 'p', 1), "the most recently used block stays");
    d.read(d.ctx, 101, 1, buf);
    check(calls == 1 && holds(buf, 'q', 1),
          "the least recently used block is the one that went");
    check(cache.evicted >= 1, "and it is counted evicted");
    check(cache.hits > 0 && cache.misses > 0, "hits and misses are counted");

    if (failed > 0) {
        printf("FAIL: %d of %d checks on the disk cache in C.\n", failed,
               passed + failed);
        return 1;
    }

    printf("PASS: %d checks on the disk cache in C (a small read answered twice "
           "with one call, written through, never added by a write, a refused "
           "write forgotten, large reads not kept, bounded by recency).\n",
           passed);
    return 0;
}
