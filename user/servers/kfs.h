/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERVERS_KFS_H
#define KOSMOS_SERVERS_KFS_H

/*
 * The on-disk filesystem in C: its format, and every operation on it
 * (`docs/diskfs.md` step 1).
 *
 * **The format is the one `user/lib/kfs.lua` made, to the byte.** A
 * superblock, a bitmap, 128-byte inodes of up to twelve extents, directories
 * as ordinary files, attributes in a block of their own, and a journal of 256
 * blocks whose commit is checksummed with FNV-1a. Every decision that places
 * a byte - which block a file takes, what a rewritten directory holds, the
 * order a commit writes in - is the one `kfs.lua` made, and the two were held
 * to that block for block until nothing ran the Lua.
 *
 * **Where a comment here says `kfs.lua` has the reasoning**, it is the Lua as
 * it was when `docs/diskfs.md` step 4 removed it - `git show
 * 48ebe67:user/lib/kfs.lua` - whose comments are the format's long-form
 * argument, beside each part. They are not repeated here.
 *
 * **No system calls, no Lua and no allocator.** It reads and writes through
 * a disk it is handed as two functions, and works in a `struct kfs` its
 * owner provides - the disk server's is static, the host's module's is
 * allocated once. That is `fat_decode.c`'s arrangement and for its reason:
 * the same file compiles on the Mac, and `tools/test_kfs.lua` asks it the 87
 * questions it asked the Lua.
 *
 * **Two functions and not four.** `kfs.lua` has four - a read and a write
 * through a string, and a read into and a write from a region - because a
 * Lua program cannot hold a pointer to a region. In C a region is mapped
 * memory, so reading a file into one is reading into a pointer, and the
 * whole blocks of a read go straight from the disk to where they belong.
 *
 * **Every result is a status**: `KFS_OK`, or a negative `KFS_E_*` that
 * `kfs_why` puts into words. What an operation answers besides comes back
 * through its pointers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KFS_MAGIC           0x4b464f53u     /* "KFOS", little endian */
#define KFS_VERSION         1u
#define KFS_BLOCK           4096u
#define KFS_SECTOR          512u
#define KFS_PER_BLOCK       (KFS_BLOCK / KFS_SECTOR)
#define KFS_INODE_SIZE      128u
#define KFS_EXTENTS         12u
#define KFS_ROOT_INODE      1u
#define KFS_JOURNAL_BLOCKS  256u

#define KFS_KIND_FREE       0u
#define KFS_KIND_FILE       1u
#define KFS_KIND_DIR        2u

#define KFS_J_MAGIC         0x4b4a524eu     /* "KJRN" */
#define KFS_J_EMPTY         0u
#define KFS_J_COMMITTED     1u

#define KFS_NAME_MAX        255u

/*
 * The most blocks one transaction changes: the journal less its header and
 * its descriptor. `kfs.lua`'s limit, and the reason for it is the same.
 */
#define KFS_TXN_MAX         (KFS_JOURNAL_BLOCKS - 2u)

/*
 * Runs of blocks one transaction may free. A block freed in a transaction is
 * not handed out again until it commits (`kfs.lua`'s `txn.freed`), and this
 * keeps them as runs rather than a flag a block. It was 512, "a few dozen
 * runs at the most" - until a file could have more extents than twelve
 * (`docs/diskfs.md`, *No limit*, G1), and freeing one of those is a run an
 * extent. A transaction that would free more is still refused rather than
 * forgetting one.
 */
#define KFS_FREED_RUNS      16384u

/*
 * **More extents than an inode holds** (`docs/diskfs.md`, *No limit*, G1;
 * Diego, 8 October: "i dont want a limit"). An inode has twelve slots. A
 * file in more pieces than that keeps eleven there, and its twelfth slot is
 * `count` 0 - which no extent ever is - with `start` the first of a chain of
 * extent blocks: 511 extents each, and the 512th slot the next block in the
 * chain, `count` 0, or zeroes at the end. `extents` counts every extent the
 * file has, wherever it is. A file of twelve or fewer is exactly what it was.
 *
 * `KFS_FILE_RUNS` is the most pieces one file is written in, which is the
 * working list a write keeps so a failure gives back exactly what it took.
 */
#define KFS_EXT_PER_BLOCK   511u
#define KFS_FILE_RUNS       16384u

/*
 * The largest directory this can edit. A directory is rewritten whole on
 * every change (`kfs.lua`'s `write_dir`), so it is held whole while it is:
 * a megabyte is some forty thousand names. Larger is refused, and never
 * truncated.
 */
#define KFS_DIR_ROOM        (1024u * 1024u)

/* A file's time: `kfs.lua`'s `stamp` and `modified`, and why is there. */
#define KFS_DATED           (1ull << 62)
#define KFS_NO_TIME         UINT64_MAX      /* "not given": keep what it had */

static inline uint64_t kfs_stamp(uint64_t epoch, uint32_t nth)
{
    return KFS_DATED | (epoch << 16) | (nth & 0xffffu);
}

/* Seconds since 1970, or false for a stamp that is a count from a boot. */
static inline bool kfs_modified(uint64_t mtime, uint64_t *epoch)
{
    if ((mtime & KFS_DATED) == 0) {
        return false;
    }

    *epoch = (mtime & ~KFS_DATED) >> 16;
    return true;
}

enum {
    KFS_OK              =   0,
    KFS_E_DISK          =  -1,
    KFS_E_NOT_KFS       =  -2,
    KFS_E_VERSION       =  -3,
    KFS_E_BLOCK_SIZE    =  -4,
    KFS_E_LAYOUT        =  -5,
    KFS_E_TOO_SMALL     =  -6,
    KFS_E_NO_FILE       =  -7,
    KFS_E_NOT_DIR       =  -8,
    KFS_E_IS_DIR        =  -9,
    KFS_E_TAKEN         = -10,
    KFS_E_NOT_EMPTY     = -11,
    KFS_E_FULL          = -12,
    KFS_E_NO_INODES     = -13,
    KFS_E_FRAGMENTED    = -14,
    KFS_E_OPEN          = -15,
    KFS_E_NOT_OPEN      = -16,
    KFS_E_TOO_BIG       = -17,
    KFS_E_DOTS          = -18,
    KFS_E_ROOT          = -19,
    KFS_E_NAME_LONG     = -20,
    KFS_E_NAME_EMPTY    = -21,
    KFS_E_INTO_ITSELF   = -22,
    KFS_E_NO_INODE      = -23,
    KFS_E_BAD_INODE     = -24,
    KFS_E_BAD_DIR       = -25,
    KFS_E_ATTRS_BIG     = -26,
    KFS_E_NOT_ATTRS     = -27,
    KFS_E_DIR_BIG       = -28,
    KFS_E_BLOCK_LONG    = -29,
};

const char *kfs_why(int status);

/* Block 0. Every number that says where something is. */
struct kfs_super {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t blocks;
    uint32_t bitmap_at;
    uint32_t bitmap_blocks;
    uint32_t inodes_at;
    uint32_t inode_count;
    uint32_t journal_at;
    uint32_t data_at;
    uint64_t created;
};

struct kfs_extent {
    uint32_t start;
    uint32_t count;
};

struct kfs_inode {
    uint32_t kind;
    uint32_t links;
    uint64_t size;
    uint64_t mtime;
    uint32_t attrs;                     /* its attribute block, or 0 */
    uint32_t extents;                   /* how many of `extent` it has */
    struct kfs_extent extent[KFS_EXTENTS];
};

/*
 * The disk: `count` blocks from `block`, to or from memory. Negative is a
 * refusal. `most` is how many blocks one call may move, and nothing asks
 * for more; 0 is taken as one.
 */
struct kfs_disk {
    void *ctx;
    int (*read)(void *ctx, uint32_t block, uint32_t count, void *to);
    int (*write)(void *ctx, uint32_t block, uint32_t count, const void *from);
    uint32_t most;
};

/*
 * Everything the filesystem works in. Large - about two megabytes, most of
 * it the transaction's blocks and a directory being edited - and its
 * owner's to place; nothing in here points outside it but `disk.ctx`.
 */
struct kfs {
    struct kfs_disk disk;

    /*
     * The transaction in progress. `journal[0]` is where the commit builds
     * the descriptor, and each block the transaction holds is in the slot
     * after it, in the order they were first written - which is the order
     * the journal gets them in, so the whole of it is one run in memory.
     */
    bool open;
    bool too_big;
    uint32_t held;
    uint32_t held_at[KFS_TXN_MAX];
    uint8_t journal[KFS_TXN_MAX + 1][KFS_BLOCK];
    uint32_t freed_runs;
    struct kfs_extent freed[KFS_FREED_RUNS];

    /* A directory's entries, without the empty ones, while it is used. */
    uint32_t dir_len;
    uint8_t dir[KFS_DIR_ROOM];

    /* Blocks of its own to work in: each has one use, so none is shared. */
    uint8_t map[KFS_BLOCK];             /* a bitmap block */
    uint8_t ino[KFS_BLOCK];             /* an inode table block */
    uint8_t part[KFS_BLOCK];            /* a block read or written in part */
    uint8_t ext[KFS_BLOCK];             /* a block of a file's extents */

    /* A file's runs while it is written (`KFS_FILE_RUNS`). */
    uint32_t runs_n;
    struct kfs_extent runs[KFS_FILE_RUNS];
    uint8_t head[KFS_BLOCK];            /* the journal's header, the superblock */
};

void kfs_init(struct kfs *k, const struct kfs_disk *disk);

/* The superblock, read and held to what one must be. */
int kfs_mount(struct kfs *k, struct kfs_super *sb);

/*
 * **For a reader that only recognises a volume** - the drive server, which
 * lists a stick's partitions and their free space and has no `struct kfs`
 * to mount one with (`docs/diskfs.md` step 4). It read the superblock and
 * the bitmap with checks and a layout of its own, a second reading of the
 * format that had to be kept in step with this one by hand; these are this
 * one's.
 *
 * Block 0's bytes, `size` of them, as a superblock - held to every check a
 * mount makes. `KFS_OK`, or why not.
 */
int kfs_super_decode(const uint8_t *block, size_t size, struct kfs_super *sb);

/* Free blocks among the first `bits` bits of an allocation bitmap - a zero
 * bit is a free block - for a caller holding the bitmap's bytes itself. */
uint64_t kfs_bitmap_free(const uint8_t *map, uint64_t bits);

/*
 * A new filesystem over `sectors`, with the folders a Kosmos disk has made in
 * it: `layout`, a list ending in NULL, or `kfs_layout` - `/Home` - when that
 * is NULL. Another list is for a suite that needs a disk made before 27
 * September, whose home was `/home` beside `/system` and `/user`.
 */
extern const char *const kfs_layout[];

int kfs_mkfs(struct kfs *k, uint64_t sectors, uint64_t now,
             const char *const *layout, struct kfs_super *sb);

/* What a mount does first: a committed transaction finished, and how many
 * blocks that took. Refused while a transaction is open. */
int kfs_recover(struct kfs *k, const struct kfs_super *sb, uint32_t *replayed);

int kfs_begin(struct kfs *k);
void kfs_rollback(struct kfs *k);

/* `stop_after_commit` returns once the commit has landed and before any of
 * it is where it belongs - the instant a power cut cannot be aimed at, for
 * the tests; nothing else passes it. */
int kfs_commit(struct kfs *k, const struct kfs_super *sb, bool stop_after_commit);

/* One block, as the open transaction has it. A write shorter than a block
 * is padded with zeroes. */
int kfs_read_block(struct kfs *k, uint32_t n, void *to);
int kfs_write_block(struct kfs *k, uint32_t n, const void *from, uint32_t len);

int kfs_alloc_run(struct kfs *k, const struct kfs_super *sb, uint32_t want,
                  uint32_t *start, uint32_t *got);
int kfs_free_run(struct kfs *k, const struct kfs_super *sb, uint32_t first,
                 uint32_t count);
int kfs_free_blocks(struct kfs *k, const struct kfs_super *sb, uint64_t *count);

int kfs_read_inode(struct kfs *k, const struct kfs_super *sb, uint32_t number,
                   struct kfs_inode *out);
int kfs_write_inode(struct kfs *k, const struct kfs_super *sb, uint32_t number,
                    const struct kfs_inode *node);
int kfs_alloc_inode(struct kfs *k, const struct kfs_super *sb, uint32_t *number);

/*
 * A window of a file into memory: `want` bytes from `offset`, cut to the
 * file's end, and how many that was. Whole blocks go from the disk straight
 * to `to`; only a block entered or left part way comes through a block of
 * this one's own.
 */
int kfs_read_range(struct kfs *k, const struct kfs_super *sb,
                   const struct kfs_inode *node, uint64_t offset, uint64_t want,
                   void *to, uint64_t *placed);

/* A file's contents replaced by `size` bytes from `bytes`, and its inode
 * written. `node` is changed to say where they went. */
int kfs_write_file(struct kfs *k, const struct kfs_super *sb, uint32_t number,
                   struct kfs_inode *node, const void *bytes, uint64_t size);

/* Attributes, as the bytes `sys.pack` made of them: this does not read them.
 * `to` holds a block; no attributes is a length of 0. Writing 0 bytes
 * removes them. */
int kfs_read_attrs(struct kfs *k, const struct kfs_super *sb,
                   const struct kfs_inode *node, void *to, uint32_t *len);
int kfs_write_attrs(struct kfs *k, const struct kfs_super *sb, uint32_t number,
                    struct kfs_inode *node, const void *bytes, uint32_t len);

/* Names, found whatever their case (`kfs.lua`'s `same_name`). */
bool kfs_same_name(const char *a, size_t alen, const char *b, size_t blen);

/* What is at a path. */
int kfs_find(struct kfs *k, const struct kfs_super *sb, const char *path,
             size_t len, uint32_t *number, struct kfs_inode *node);

/* The directory that would hold it, and the name it would have there - a
 * pointer into `path`. */
int kfs_parent_of(struct kfs *k, const struct kfs_super *sb, const char *path,
                  size_t len, uint32_t *number, struct kfs_inode *node,
                  const char **name, size_t *name_len);

/* A path as this disk spells it, into `out`, which holds `len` + 2. */
size_t kfs_spelled(struct kfs *k, const struct kfs_super *sb, const char *path,
                   size_t len, bool keep_last, char *out);

/*
 * A directory's entries, held in `k->dir` for `kfs_dir_next` to walk from a
 * `pos` of 0 - in the order they are stored, and valid until the next call
 * that reads a directory. `kfs_list` is the same for a path.
 */
int kfs_open_dir(struct kfs *k, const struct kfs_super *sb,
                 const struct kfs_inode *dir);
int kfs_list(struct kfs *k, const struct kfs_super *sb, const char *path,
             size_t len);
bool kfs_dir_next(const struct kfs *k, uint32_t *pos, uint32_t *inode,
                  const char **name, uint32_t *name_len);

int kfs_mkdir(struct kfs *k, const struct kfs_super *sb, const char *path,
              size_t len, uint64_t now);

/* A file made or replaced; its inode's number. `now` may be KFS_NO_TIME. */
int kfs_store(struct kfs *k, const struct kfs_super *sb, const char *path,
              size_t len, const void *bytes, uint64_t size, uint64_t now,
              uint32_t *number);

/* `to` is a name in the same directory, or a path anywhere on this disk. */
int kfs_rename(struct kfs *k, const struct kfs_super *sb, const char *path,
               size_t len, const char *to, size_t to_len);

int kfs_unlink(struct kfs *k, const struct kfs_super *sb, const char *path,
               size_t len);

#endif /* KOSMOS_SERVERS_KFS_H */
