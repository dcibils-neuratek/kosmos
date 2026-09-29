/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The disk's small reads, kept (`diskcache.h`).
 */

#include "diskcache.h"

#include <string.h>

void diskcache_init(struct diskcache *c, const struct kfs_disk *under,
                    uint32_t most, uint32_t small)
{
    c->under = *under;
    c->most = most < DISKCACHE_MOST ? most : DISKCACHE_MOST;
    c->small = small;
    c->count = 0;
    c->clock = 0;
    c->hits = c->misses = c->evicted = 0;
}

void diskcache_clear(struct diskcache *c)
{
    c->count = 0;
}

static int slot_of(const struct diskcache *c, uint32_t block)
{
    for (uint32_t i = 0; i < c->count; i++) {
        if (c->block[i] == block) {
            return (int)i;
        }
    }

    return -1;
}

static void keep(struct diskcache *c, uint32_t block, const uint8_t *bytes)
{
    int slot = slot_of(c, block);

    if (slot < 0) {
        if (c->count < c->most) {
            slot = (int)c->count++;
        } else {
            slot = 0;

            for (uint32_t i = 1; i < c->count; i++) {
                if (c->used[i] < c->used[slot]) {
                    slot = (int)i;
                }
            }

            c->evicted++;
        }

        c->block[slot] = block;
    }

    memcpy(c->data[slot], bytes, KFS_BLOCK);
    c->used[slot] = ++c->clock;
}

static void forget(struct diskcache *c, uint32_t block)
{
    int slot = slot_of(c, block);

    if (slot >= 0) {
        c->count--;
        c->block[slot] = c->block[c->count];
        c->used[slot] = c->used[c->count];
        memcpy(c->data[slot], c->data[c->count], KFS_BLOCK);
    }
}

static int cache_read(void *ctx, uint32_t block, uint32_t count, void *to)
{
    struct diskcache *c = ctx;
    uint8_t *out = to;
    int r;

    if (count > c->small) {
        return c->under.read(c->under.ctx, block, count, to);
    }

    for (uint32_t i = 0; i < count; i++) {
        if (slot_of(c, block + i) < 0) {
            goto miss;
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        int slot = slot_of(c, block + i);

        memcpy(out + (size_t)i * KFS_BLOCK, c->data[slot], KFS_BLOCK);
        c->used[slot] = ++c->clock;
    }

    c->hits++;
    return 0;

miss:
    c->misses++;
    r = c->under.read(c->under.ctx, block, count, to);

    if (r >= 0) {
        for (uint32_t i = 0; i < count; i++) {
            keep(c, block + i, out + (size_t)i * KFS_BLOCK);
        }
    }

    return r;
}

static int cache_write(void *ctx, uint32_t block, uint32_t count, const void *from)
{
    struct diskcache *c = ctx;
    const uint8_t *bytes = from;
    int r = c->under.write(c->under.ctx, block, count, from);

    for (uint32_t i = 0; i < count; i++) {
        int slot = slot_of(c, block + i);

        if (slot < 0) {
            continue;
        }

        if (r >= 0) {
            memcpy(c->data[slot], bytes + (size_t)i * KFS_BLOCK, KFS_BLOCK);
        } else {
            forget(c, block + i);
        }
    }

    return r;
}

struct kfs_disk diskcache_disk(struct diskcache *c)
{
    struct kfs_disk d = { c, cache_read, cache_write, c->under.most };

    return d;
}
