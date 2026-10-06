/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DISKPROTO_H
#define KOSMOS_DISKPROTO_H

#include <stdint.h>

/*
 * The disk: `/Home`, a kfs volume, served by `user/servers/diskfs.c`
 * (`docs/diskfs.md` step 3).
 *
 * `ramproto.h`'s conventions, for `drivesproto.h`'s reason turned round: a
 * listing a page at a time from an offset, a read of bytes from an offset
 * with `more`, an error as a number the namespace puts into words. **A
 * protocol of its own** rather than `/Temporary`'s, because `/Temporary`
 * has `watch`, which the disk does not, and the disk has three things
 * `/Temporary` does not:
 *
 *   - **regions**: a read into a region handed with the request, and a
 *     write from one - a file's bytes never travel in a message
 *     (`CLAUDE.md`, control by message, data by shared memory), except the
 *     kilobyte a small read or write fits in;
 *   - **the disk itself** - `.super`, what it is and how full, `.device`,
 *     what it has cost, and `.format` - as structures of their own;
 *   - **attributes of any length**: a launcher's program and arguments do
 *     not fit `/Temporary`'s forty-eight characters, so attributes cross as
 *     the bytes `sys.pack` makes, which is also how they are stored.
 *
 * **Values are the namespace's.** `fs.write(path, table)` stores the table
 * packed, with a mark in front, and a read gives it back; the server stores
 * and returns bytes and never knows.
 */

#define DISK_OP_LIST      1u    /* a directory's names, a page from `offset` */
#define DISK_OP_READ      2u    /* bytes from `offset`: into the region, or a page */
#define DISK_OP_WRITE     3u    /* a file replaced: from the region, or `u.data` */
#define DISK_OP_DELETE    4u
#define DISK_OP_RENAME    5u    /* to `u.to`: a name, or a path on this disk */
#define DISK_OP_MKDIR     6u
#define DISK_OP_GETATTR   7u    /* `node`, and the attributes a page from `offset` */
#define DISK_OP_SETATTR   8u    /* `u.data`: the changes, packed; "" removes */
#define DISK_OP_QUERY     9u    /* `u.data`: the terms, packed; paths a page at a time */
#define DISK_OP_SUPER    10u    /* `u.super` */
#define DISK_OP_DEVICE   11u    /* `u.device` */
#define DISK_OP_FORMAT   12u    /* `u.data`: the words that say it may */

/* The request's region is the bytes: read into it, or written from it. */
#define DISK_REGION       1u

#define DISK_OK                 0u
#define DISK_ERR_BAD_OP         1u  /* no such operation, the wrong size, a path with no end */
#define DISK_ERR_NO_FS          2u  /* no filesystem this server can read */
#define DISK_ERR_RESERVED       3u  /* `.super`, `.device` or `.format` */
#define DISK_ERR_THE_DIRECTORY  4u  /* a read of a directory's own path */
#define DISK_ERR_DERIVED        5u  /* setattr of what a node *is*; its name in `u.data` */
#define DISK_ERR_NOT_FLAT       6u  /* attributes or terms that are not a flat table */
#define DISK_ERR_REGION         7u  /* no region with it, or one too small */
#define DISK_ERR_WORDS          8u  /* a format that did not say "yes, erase it" */
#define DISK_ERR_NO_DISK        9u  /* no disk; why in `u.data` */
#define DISK_ERR_ATTRS_BIG     10u  /* more attributes than a node holds */
#define DISK_ERR_SEARCH        11u  /* more folders than one query holds */
#define DISK_ERR_ANSWERS       12u  /* more answers than one query keeps */

/*
 * **A share's three** (`docs/sharing.md`, *A share is a disk*): a server
 * that is not `/Home` says why in `u.data`, `length` bytes, and the
 * namespace shows those words - a mount's name in its own sentences, where
 * `/Home`'s numbers are put into words by the namespace.
 */
#define DISK_ERR_READ_ONLY     13u  /* "diego-mac's Projects is open read only" */
#define DISK_ERR_AWAY          14u  /* "diego-mac is not answering" */
#define DISK_ERR_DENIED        15u  /* the server refused this file to this account */

/*
 * And the filesystem's own refusals, as `DISK_ERR_KFS` plus `-KFS_E_*`
 * (`user/servers/kfs.h`): "no such file", "that name is taken", "the disk
 * is full" and the rest, one number each.
 */
#define DISK_ERR_KFS           32u

#define DISK_KIND_FILE    1u    /* kfs's own numbers */
#define DISK_KIND_DIR     2u
#define DISK_KIND_DEVICE  3u    /* `.super` and its siblings, and the root */

/*
 * **A path is 512 bytes**, twice `/Temporary`'s: a disk holds folders deep
 * enough for a whole path of names of sixty-four characters to pass 256.
 * Ends with a zero byte inside the field, or the request is refused.
 */
#define DISK_PATH_MAX   512u
#define DISK_DATA_MAX  1024u

/*
 * How a stick's `/Home` was found, a count of looks that stopped at each
 * step - the steps the namespace names (`STEPS` beside `disk_request`).
 */
#define DISK_STICK_STEPS 11u

struct disk_request {
    uint32_t op;
    uint32_t flags;             /* DISK_REGION */
    uint64_t offset;            /* a page's first entry; a read's first byte */
    uint64_t bytes;             /* into or from the region: how many */
    uint32_t length;            /* bytes of `u` in use */
    uint32_t reserved;
    char     path[DISK_PATH_MAX];

    union {
        char data[DISK_DATA_MAX];
        char to[DISK_PATH_MAX];
    } u;
};

/* What a node is: `getattr`'s facts, read out of the inode. */
struct disk_node {
    uint32_t kind;              /* DISK_KIND_* */
    uint32_t extents;
    uint64_t size;
    uint64_t mtime;             /* the stamp */
    uint64_t modified;          /* seconds since 1970, when `dated` */
    uint32_t dated;
    uint32_t reserved;
};

/* `.super`: the disk and the filesystem on it. */
struct disk_super {
    uint64_t sectors;
    uint64_t bytes;
    uint32_t sector_size;
    uint32_t present;           /* there is a disk */
    uint32_t formatted;         /* with a filesystem this server reads */
    uint32_t free_known;        /* `free_blocks` was counted; else `free_why` */
    uint64_t free_blocks;

    /* The superblock, when `formatted`. */
    uint32_t magic, version, block_size, blocks;
    uint32_t bitmap_at, bitmap_blocks, inodes_at, inode_count;
    uint32_t journal_at, data_at;
    uint64_t created;

    /* A stick's `/Home`: whether one was looked for, and how that went. */
    uint32_t searched;
    uint32_t looks;
    uint32_t stops[DISK_STICK_STEPS];
    uint32_t reserved;
    uint64_t first, found;      /* counter ticks: the first look, and the one that found it */

    char why[128];              /* not present, or not formatted: why */
    char where[128];            /* which partition, on which stick */
    char flush_why[96];         /* the stick's refusal of a flush */
    char free_why[96];
};

/*
 * `.device`: what the device has cost this server so far.
 *
 * A share's (smbfs): `reads` and `read_bytes` are SMB READs and what they
 * brought, `read_counter_ticks` the time callers waited on them;
 * `cache_hits` the questions about names answered from memory and
 * `cache_misses` the folders asked of the server; and the last four are
 * the network's - every SMB request sent, the folder listings asked for,
 * the requests those took and the time they took.
 */
struct disk_device {
    uint64_t reads, writes;
    uint64_t read_bytes, write_bytes;
    uint64_t read_counter_ticks, write_counter_ticks;
    uint64_t cache_hits, cache_misses;
    uint64_t requests;          /* a share's: SMB requests sent */
    uint64_t listings;          /* a share's: folder listings asked for */
    uint64_t listing_requests;  /* the SMB requests those took */
    uint64_t listing_counter_ticks;
};

struct disk_reply {
    uint32_t error;
    uint32_t more;              /* another page follows, from `offset` */
    uint32_t count;             /* names in the page */
    uint32_t length;            /* bytes of `u.data` */
    uint64_t offset;            /* where the next page starts */
    uint64_t bytes;             /* read into, or written from, the region */
    uint64_t size;              /* the file's size, on a read */
    struct disk_node node;      /* getattr */

    /*
     * A read's bytes; getattr's attributes, packed, a page; a page of names
     * or paths, each ending in a zero byte; a refused name; or the disk.
     */
    union {
        char data[DISK_DATA_MAX];
        struct disk_super super;
        struct disk_device device;
    } u;

    /*
     * **Whether the answer is as it was last heard** (`docs/sharing.md`
     * step N3): a listing or a node's facts from a share's server that did
     * not answer within its bound, given from memory - and how long ago
     * that server last answered anything, in milliseconds, so no clock's
     * ticks cross. `/Home` always answers fresh and sets neither. After
     * `u`, so a reader of the fields before it reads them where it did.
     */
    uint32_t heard;             /* 1: from memory, the server silent */
    uint32_t reserved2;
    uint64_t heard_ms;          /* how long since the server last answered */
};

_Static_assert(sizeof(struct disk_node) == 40, "disk_node has no padding");
_Static_assert(sizeof(struct disk_super) <= DISK_DATA_MAX,
               "disk_super fits where a page of bytes does");
_Static_assert(sizeof(struct disk_request) <= 2048,
               "a disk request must fit in one message");
_Static_assert(sizeof(struct disk_reply) == 1120,
               "a disk reply is the size the namespace unpacks");
_Static_assert(sizeof(struct disk_reply) <= 2048,
               "a disk reply must fit in one message");

#endif /* KOSMOS_DISKPROTO_H */
