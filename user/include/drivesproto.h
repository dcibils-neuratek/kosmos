/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVESPROTO_H
#define KOSMOS_DRIVESPROTO_H

#include <stdint.h>

/*
 * /drives: every volume on every drive, read only (USB step 6b,
 * `docs/drives.html`).
 *
 * `audioproto.h`'s shape for `audioproto.h`'s reason - fixed fields, fixed
 * sizes, an error as a number - and `ramproto.h`'s conventions where the two
 * are doing the same job, because a listing that pages and a read that takes
 * an offset are solved problems and a second answer to either would be a
 * second thing to get right.
 *
 * **Its own protocol rather than ramfs's, and the reason is honesty.** A
 * read-only drive server would implement three of `ramproto.h`'s eleven
 * operations and refuse eight, and `ramproto.h` says in its own comment that
 * it is a filesystem *and* an attribute store *and* a query engine. Sharing
 * it would mean two servers speaking a protocol named after one of them,
 * where the second only pretends to most of it.
 *
 * **One server owns the whole prefix**, rather than a mount per volume.
 * `drives.html` says `/drives` is "one folder every program has from the
 * moment it starts", with drives appearing and disappearing inside it while
 * programs run - and a mount is an entry in a process's own namespace, made
 * when that process is built. A volume appearing later would mean editing
 * the namespace of every running program, which nothing can do. A server
 * behind one prefix needs none of that: the matcher routes
 * `/drives/PHOTOS 2024/Italy` to it with the rest of the path intact,
 * exactly as `/home` is routed today.
 *
 * **Nothing here writes.** There is no write, no delete, no rename, and the
 * server is given the USB driver's *read* endpoint and never the write one -
 * so read-only is what this process can do rather than what it agrees to do.
 * Writing to another machine's filesystem comes later and deliberately
 * (`README.md`).
 */

#define DRIVES_OP_VOLUMES   1u  /* what is plugged in, with sizes */
#define DRIVES_OP_LIST      2u  /* a directory, a page at a time */
#define DRIVES_OP_READ      3u  /* bytes of a file, from an offset */
#define DRIVES_OP_GETATTR   4u  /* one entry: its size, and whether it is a
                                 * directory */

#define DRIVES_OK              0u
#define DRIVES_ERR_NO_PATH     1u  /* no such volume, directory or file */
#define DRIVES_ERR_NOT_DIR     2u  /* a path component that is not a directory */
#define DRIVES_ERR_BAD_OP      3u  /* no such operation, or the wrong size */
#define DRIVES_ERR_DEVICE      4u  /* the drive would not read */
#define DRIVES_ERR_UNREADABLE  5u  /* a volume whose filesystem this cannot
                                    * read: NTFS, ext4, exFAT until 6f - and
                                    * kfs, whose reader is Lua */
#define DRIVES_ERR_DAMAGED     6u  /* a chain that leads nowhere, or a
                                    * directory that does not end */

/*
 * A path is 256 bytes and a name 64, which are `ramproto.h`'s numbers and
 * are its reasoning too: a name has to hold sixty-four characters, and a
 * whole path two directories down has to hold a name of that size.
 *
 * `DRIVES_NAME_BYTES` is repeated from `drives_decode.h` rather than
 * included from it, because this header is compiled by clients that have no
 * business knowing how a partition table is read. The `_Static_assert` at the
 * foot of `drives.c` holds the two together.
 */
#define DRIVES_PATH_MAX      256u
#define DRIVES_NAME_BYTES     64u
#define DRIVES_ENTRIES_MAX     4u   /* names in one page of a listing */
#define DRIVES_DATA_MAX     1024u   /* bytes of a file in one message */
#define DRIVES_VOLUMES_MAX     8u   /* volumes in one page of a listing */

/* Which filesystem a volume holds. The same order as `enum drives_fs` in
 * `drives_decode.h`, and `drives.c` asserts they have not drifted. */
#define DRIVES_FS_NONE     0u
#define DRIVES_FS_FAT16    1u
#define DRIVES_FS_FAT32    2u
#define DRIVES_FS_KFS      3u
#define DRIVES_FS_OTHER    4u

/*
 * One volume, as the Drives list shows it (`drives.html`).
 *
 * `free_bytes` is worth a note, because two of the three filesystems answer
 * it differently and the difference is visible to a person. kfs keeps no free
 * count, so it is added up out of the allocation bitmap - one block to read
 * on a 32 MB volume, and exact. FAT32 keeps a *hint* in its FSInfo sector,
 * and counting properly means reading the whole table: 8 MB on a 64 GB
 * volume, about four seconds on the ThinkPad's stick at the 2.1 MB/s
 * `diskbench` measured. A sidebar cannot wait four seconds, so the hint is
 * used when it is there and `free_exact` says which was given. FAT16's table
 * is small enough to count outright.
 *
 * `unit` and `partition` are the stable handle. A *name* depends on the order
 * drives arrived - Diego took that cost deliberately on 16 September - so
 * anything that must still mean the same volume after a replug uses these
 * two rather than the path.
 */
struct drives_volume {
    char     name[DRIVES_NAME_BYTES];   /* `PHOTOS 2024`, `Untitled 2` */
    uint32_t fs;                        /* DRIVES_FS_* */
    uint32_t free_exact;                /* 1 counted, 0 FAT32's hint */
    uint64_t bytes;                     /* the volume's size */
    uint64_t free_bytes;
    uint32_t unit;                      /* which drive, as the USB driver
                                         * numbers them */
    uint32_t partition;                 /* which partition on it, from 0 */
    uint32_t readable;                  /* whether this server can open it */
    uint32_t reserved;                  /* keeps the struct a multiple of 8 */
};

/*
 * One entry in a directory listing.
 *
 * A name rather than a whole path, because the caller knows which directory
 * it asked about - and four whole paths would be the entire message where
 * four names leave room for the sizes beside them, which is what a Tracker
 * column needs and what a second request per entry would otherwise cost.
 */
struct drives_entry {
    char     name[DRIVES_NAME_BYTES];
    uint64_t size;                      /* bytes; 0 for a directory */
    uint32_t directory;
    uint32_t reserved;
};

struct drives_request {
    uint32_t op;
    uint32_t offset;            /* which page of a listing */
    uint64_t at;                /* which byte of a read */
    uint32_t length;            /* bytes wanted, up to DRIVES_DATA_MAX */
    uint32_t reserved;

    /* The path under `/drives`, as the namespace hands it over: the volume's
     * name first, then whatever is inside it. Empty, or `/`, is the list of
     * volumes itself. */
    char     path[DRIVES_PATH_MAX];
};

struct drives_reply {
    uint32_t error;             /* DRIVES_OK, or why not */
    uint32_t more;              /* another page follows this one */
    uint32_t count;             /* entries or volumes in `u` */
    uint32_t length;            /* bytes of `u.data` */

    /* For getattr, and for a read: what the whole thing is. */
    uint64_t size;
    uint32_t directory;
    uint32_t reserved;

    union {
        char                 data[DRIVES_DATA_MAX];
        struct drives_entry  entries[DRIVES_ENTRIES_MAX];
        struct drives_volume volumes[DRIVES_VOLUMES_MAX];
    } u;
};

/*
 * Both must fit in one message, and `MSG_BYTES` is 2048.
 *
 * Checked rather than assumed, for `audioproto.h`'s reason: the counts above
 * are what somebody raises without thinking, and the failure would be a reply
 * silently truncated rather than a build that stops.
 */
_Static_assert(sizeof(struct drives_request) <= 2048,
               "a /drives request must fit in one message");
_Static_assert(sizeof(struct drives_reply) <= 2048,
               "a /drives reply must fit in one message - lower "
               "DRIVES_VOLUMES_MAX or DRIVES_ENTRIES_MAX");

#endif /* KOSMOS_DRIVESPROTO_H */
