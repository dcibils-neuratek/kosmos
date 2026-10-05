/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERVERS_DRIVES_DECODE_H
#define KOSMOS_SERVERS_DRIVES_DECODE_H

/*
 * What a drive's sectors say about the volumes on it, for the drive server
 * (USB step 6b, `docs/drives.html`): where the partitions are, which
 * filesystem each one holds, and what to call it.
 *
 * Its own file, with no hardware and no system calls in it, for
 * `fat_decode.c`'s reason and `storage_decode.c`'s before it: the awkward
 * cases are all about bytes, and `tools/test_drivesdecode.c` can ask about
 * them on the Mac, a byte at a time, without a machine to boot.
 *
 * **It decides nothing about reading a file.** `fat_decode.h` is what knows
 * what a FAT volume's bytes mean; this is what finds the volumes and names
 * them, which is the part that has to happen before any of that.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fat_decode.h"

/*
 * A shown name: 63 bytes and a terminator.
 *
 * A FAT label is eleven characters and kfs keeps none at all, so this is not
 * about what a volume can be called - it is room for the numbering below to
 * work in, and for exFAT's fifteen UTF-16 characters when 6f arrives.
 */
#define DRIVES_NAME_BYTES       64u

/* Partitions considered on one drive. Four is MBR's whole table and 128 is
 * what a GPT conventionally has room for; anything past this is not shown
 * rather than being a reason to refuse the drive. */
#define DRIVES_PARTS_MAX        32u

/* MBR: the table's first entry, each entry's length, and the signature. */
#define MBR_TABLE_AT            446u
#define MBR_ENTRY_BYTES         16u
#define MBR_ENTRIES             4u

/*
 * The partition types that hold a FAT this reads. Microsoft's list; the two
 * `_LBA` ones are what anything modern writes, and `tools/test_fat.py`
 * already builds its MBR volumes with 0x0C and 0x0E.
 *
 * A type is a *hint* and never the answer: the boot sector decides, because
 * `fat_volume_from` holds it to the specification and a type byte is one
 * byte somebody may have written by hand. A type not in this list is still
 * offered to the boot sector, so a FAT volume under an unexpected type is
 * found; the list exists to skip the obvious non-FAT ones cheaply.
 */
#define MBR_TYPE_FAT16          0x06u
#define MBR_TYPE_FAT32          0x0Bu
#define MBR_TYPE_FAT32_LBA      0x0Cu
#define MBR_TYPE_FAT16_LBA      0x0Eu
#define MBR_TYPE_EXTENDED       0x05u
#define MBR_TYPE_EXTENDED_LBA   0x0Fu
#define MBR_TYPE_GPT_PROTECTIVE 0xEEu

/* Which filesystem a volume holds, as far as this can tell from its first
 * sectors. `FS_KIND_OTHER` is a partition that exists and holds something
 * this does not read - NTFS, ext4, exFAT until 6f - which is shown rather
 * than hidden, because a drive with one partition missing looks broken. */
/*
 * **Named `FS_KIND_*` rather than `DRIVES_FS_*`, and the collision is worth
 * recording.** `drivesproto.h` carries the same five values as *macros*,
 * because they travel on the wire and a client has no business including
 * this file. A server includes both, and the preprocessor rewrote this
 * enum's declaration into bare numbers - so the two must not share a
 * spelling. `drives.c` asserts they still agree.
 */
enum drives_fs {
    FS_KIND_NONE,             /* no partition there at all */
    FS_KIND_FAT16,
    FS_KIND_FAT32,
    FS_KIND_KFS,
    FS_KIND_OTHER,
};

/*
 * One partition, in the drive's own sectors.
 *
 * `first` and `sectors` are what the table said. Whether they fit the drive
 * is the caller's to check, because only the caller has asked the device how
 * big it is - the same division `fat_volume_from` draws for the same reason.
 */
struct drives_part {
    uint64_t first;             /* its first sector, from the drive's zero */
    uint64_t sectors;           /* how many it says it has */
    uint8_t  type;              /* MBR's type byte; 0 for a GPT partition */
    bool     gpt;               /* which table it came out of */
    bool     has_guid;          /* only a GPT partition has one */
    uint8_t  guid[16];          /* UniquePartitionGUID, as on disk */
    uint8_t  type_guid[16];     /* PartitionTypeGUID, as on disk; GPT only */
};

/*
 * The partitions an MBR names: `sector` is the drive's first, `size` bytes of
 * it. Entries of no sectors are skipped. Returns how many were written.
 *
 * **A protective MBR is not a partition**, and this returns none for a table
 * whose only entry is type 0xEE - the caller reads the GPT instead. That is
 * what a GPT drive looks like from here, and treating the protective entry as
 * a volume would offer the whole drive as one.
 *
 * Extended partitions are named and not followed: `type` keeps 0x05 or 0x0F
 * and `drives_fs_of` calls them OTHER. Following the chain is more sectors to
 * read and no drive Diego owns needs it.
 */
unsigned mbr_partitions(const uint8_t *sector, unsigned size,
                        struct drives_part *out, unsigned most);

/* Whether an MBR's table is only a protective entry, which means the drive's
 * real table is the GPT. */
bool mbr_is_protective(const uint8_t *sector, unsigned size);

/*
 * The partitions a GPT's entry array names. `entries` is `bytes` of it,
 * `entry_size` and `count` from the header - both are the header's word and
 * are held to something sane here. An entry whose type GUID is all zeroes is
 * unused and skipped; the others carry it, so a caller looking for one kind
 * of partition - `diskfs` for Kosmos's own - reads the table here rather
 * than a second time.
 */
unsigned gpt_partitions(const uint8_t *entries, unsigned bytes,
                        unsigned entry_size, unsigned count,
                        struct drives_part *out, unsigned most);

/*
 * A GUID as it is written out - `8A9DC8A8-83CF-4F7F-962B-43157A68F14A` - from
 * its sixteen bytes as a GPT keeps them: the first three fields
 * little-endian, the last two as they are, upper-case hex. `out` holds 36
 * characters and a terminator.
 */
void drives_guid_text(const uint8_t guid[16], char out[37]);

/*
 * Where a GPT header says its entry array is, and how it is shaped. False for
 * a header `gpt_header_at` would refuse, or one whose array is too large to
 * be read in one go by the caller's standards - which the caller says with
 * `most_bytes`.
 */
bool gpt_entry_array(const uint8_t *header, unsigned size, unsigned most_bytes,
                     uint64_t *at, unsigned *entry_size, unsigned *count);

/*
 * A Kosmos volume is read with `kfs.c`'s own `kfs_super_decode` and
 * `kfs_bitmap_free` (`kfs.h`). This file read the superblock and the bitmap
 * itself, with a layout and checks written down a second time beside
 * `kfs.lua`'s - which `docs/diskfs.md` step 4 retired with `kfs.lua`.
 */

/*
 * FAT32's FSInfo sector: a *hint* at how many clusters are free.
 *
 * **A hint, and this is why it is used anyway.** Counting FAT32's free
 * clusters properly means reading the whole table - 8 MB on a 64 GB volume,
 * which is about 10 ms under QEMU and four seconds on the ThinkPad's stick at
 * the 2.1 MB/s `diskbench` measured. A sidebar cannot wait four seconds, and
 * a number that is occasionally stale is better there than a spinner.
 *
 * So the hint is read when it is there and believed when it is possible, and
 * `drives_free` says which it gave. `FSI_LeadSig` and the sector's place in
 * `BPB_FSInfo` are the fields `tools/test_fat.py` already reads to find
 * mtools's own FSInfo; 0xFFFFFFFF is the specification's "unknown".
 */
#define FAT_FSINFO_LEAD         0x41615252u
#define FAT_FSINFO_STRUCT       0x61417272u
#define FAT_FSINFO_TRAIL        0xAA550000u
#define FAT_FSINFO_UNKNOWN      0xFFFFFFFFu

struct fat_fsinfo {
    uint32_t free_clusters;     /* FSI_Free_Count */
    uint32_t next_free;         /* FSI_Nxt_Free */
    bool     free_known;        /* it was there and was not 0xFFFFFFFF */
};

/* Which sector of the volume holds the FSInfo, out of BPB_FSInfo. 0 when the
 * boot sector names none, which is every FAT16 volume. */
uint32_t fat_fsinfo_sector(const uint8_t *boot, unsigned size,
                           const struct fat_volume *v);

/* The FSInfo sector's three signatures and its two counts. False for a sector
 * that is not one. */
bool fat_fsinfo_from(const uint8_t *sector, unsigned size,
                     struct fat_fsinfo *out);

/*
 * The name a volume is shown under.
 *
 * Diego's answers, 16 September, to the two questions `drives.html` left
 * open. A volume with no label is `Untitled` - Finder's word, and the one a
 * person already recognises. `label` is trusted for nothing: anything outside
 * printable ASCII becomes `_`, and `/` would make a second path component so
 * it goes too.
 */
void drives_label_name(char *out, unsigned bytes, const char *label);

/*
 * Make `name` one that `taken` does not already hold: `PHOTOS`, then
 * `PHOTOS 2`, then `PHOTOS 3`. Compared without regard to case, because FAT
 * finds names that way and two drives differing only in case would be two
 * paths a person cannot tell apart.
 *
 * The same rule settles the unlabelled collision, so two blank sticks are
 * `Untitled` and `Untitled 2`.
 *
 * **What this costs, and Diego took it deliberately**: the number depends on
 * the order drives arrived, so pulling the first `PHOTOS` and plugging it
 * back can make it `PHOTOS 2`. A path is therefore not stable across a
 * replug, and anything wanting a stable handle uses the unit and partition.
 */
void drives_unique_name(char *name, unsigned bytes,
                        const char *const *taken, unsigned count);

#endif /* KOSMOS_SERVERS_DRIVES_DECODE_H */
