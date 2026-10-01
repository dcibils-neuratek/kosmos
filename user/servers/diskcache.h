/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERVERS_DISKCACHE_H
#define KOSMOS_SERVERS_DISKCACHE_H

/*
 * **The disk's small reads, kept**, for the disk server in C (`docs/diskfs.md`
 * step 3) - the rules `blockcache.lua` kept for the disk server in Lua until
 * step 4 removed both, and `design.md` 8.3d's reason:
 * a 4 KB read of a file in `/Home` was six disk calls, and all but one were
 * its path - inode blocks and directories read again for every request.
 *
 * **A disk over a disk.** It is a `struct kfs_disk` whose two functions go to
 * the device underneath, so `kfs.c` reads and writes through it without
 * knowing, and every block the filesystem moves passes it.
 *
 * - **Only what the device holds.** A write goes to the device first; a
 *   block kept here is then replaced by what was written, or forgotten when
 *   the write was refused. A write never adds a block: what is kept is what
 *   was read.
 * - **Small reads only** - `small` blocks or fewer: an inode block, a bitmap
 *   block, a directory, a settings file. A file read in one large piece
 *   passes by, rather than pushing all of that out for bytes nobody reads
 *   twice.
 * - **Bounded**: `most` blocks, the least recently used going first.
 *
 * `tools/test_diskcache.c` holds it to the checks `test_blockcache.lua` held
 * the Lua to.
 */

#include <stdint.h>

#include "kfs.h"

#define DISKCACHE_MOST 64u

struct diskcache {
    struct kfs_disk under;
    uint32_t most;                      /* at most DISKCACHE_MOST */
    uint32_t small;
    uint32_t count;
    uint64_t clock;
    uint32_t block[DISKCACHE_MOST];
    uint64_t used[DISKCACHE_MOST];
    uint8_t data[DISKCACHE_MOST][KFS_BLOCK];

    /* Reads answered here, those passed on, and blocks pushed out. */
    uint64_t hits, misses, evicted;
};

void diskcache_init(struct diskcache *c, const struct kfs_disk *under,
                    uint32_t most, uint32_t small);

/* The disk to hand `kfs.c`: this cache, over `under`. */
struct kfs_disk diskcache_disk(struct diskcache *c);

/* Everything forgotten, for a caller that changed the device behind it. */
void diskcache_clear(struct diskcache *c);

#endif /* KOSMOS_SERVERS_DISKCACHE_H */
