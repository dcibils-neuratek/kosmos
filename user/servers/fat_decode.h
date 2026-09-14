/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERVERS_FAT_DECODE_H
#define KOSMOS_SERVERS_FAT_DECODE_H

/*
 * What the bytes of a FAT volume mean, for the drive server (USB step 6,
 * `docs/drives.html`): a boot sector read as a BPB, which table entry says
 * where a file goes next, and a directory's 32-byte entries - short names,
 * long names, and the checksum that ties the two together.
 *
 * **Every rule is Microsoft's**: the FAT32 File System Specification, version
 * 1.03 of 6 December 2000 ("fatgen103"), and the section each comes from is
 * named beside it in `fat_decode.c`. Its own file, with no hardware and no
 * system calls in it, for `storage_decode.c`'s reason: `tools/test_fatdecode.c`
 * asks it the awkward questions a byte at a time, and `tools/test_fat.py`
 * holds it to volumes mtools made - a second reading of the format, by
 * somebody else.
 *
 * FAT16 and FAT32 are read. FAT12 is recognised and refused. **Read only**:
 * nothing here writes, and nothing that uses it can.
 */

#include <stdbool.h>
#include <stdint.h>

#define FAT_DIRENT_BYTES        32u

/*
 * A long name as UTF-8, and its end. Twenty long entries of thirteen UTF-16
 * units are 260 units, three bytes at most each; the specification's limit is
 * 255 characters, and a damaged volume is not bound by it.
 */
#define FAT_NAME_BYTES          784u

/* DIR_Attr - FAT Directory Structure. */
#define FAT_ATTR_READ_ONLY      0x01u
#define FAT_ATTR_HIDDEN         0x02u
#define FAT_ATTR_SYSTEM         0x04u
#define FAT_ATTR_VOLUME_ID      0x08u
#define FAT_ATTR_DIRECTORY      0x10u
#define FAT_ATTR_ARCHIVE        0x20u
#define FAT_ATTR_LONG_NAME      0x0Fu
#define FAT_ATTR_LONG_NAME_MASK 0x3Fu

enum fat_kind {
    FAT_NONE,               /* not a FAT boot sector */
    FAT_12,
    FAT_16,
    FAT_32,
};

struct fat_volume {
    enum fat_kind kind;
    uint32_t bytes_per_sector;          /* BPB_BytsPerSec */
    uint32_t sectors_per_cluster;       /* BPB_SecPerClus */
    uint32_t cluster_bytes;
    uint32_t reserved_sectors;          /* BPB_RsvdSecCnt */
    uint32_t fats;                      /* BPB_NumFATs */
    uint32_t fat_sectors;               /* one FAT: BPB_FATSz16, or BPB_FATSz32 */
    uint32_t root_entries;              /* BPB_RootEntCnt: FAT16's fixed root */
    uint32_t root_sectors;              /* RootDirSectors */
    uint32_t root_cluster;              /* BPB_RootClus: FAT32's root */
    uint32_t total_sectors;             /* BPB_TotSec16, or BPB_TotSec32 */
    uint32_t first_data_sector;         /* FirstDataSector */
    uint32_t clusters;                  /* CountofClusters */
    char     label[12];                 /* BS_VolLab, spaces trimmed; "" for NO NAME */
};

/*
 * The volume a boot sector describes, held to what the specification says one
 * must be. False, with `why`, for anything that is not - and for FAT12, which
 * `kind` names and nothing here reads. Sectors are counted from the volume's
 * own first sector; whether `total_sectors` fits the partition it was found
 * in is the caller's to check, since only the caller knows the partition.
 */
bool fat_volume_from(const uint8_t *sector, unsigned size,
                     struct fat_volume *out, const char **why);

const char *fat_kind_name(enum fat_kind kind);

/*
 * Where `cluster`'s entry is in the first FAT: its sector, and the byte in it.
 * False for a number that is not one of the volume's data clusters.
 */
bool fat_entry_place(const struct fat_volume *v, uint32_t cluster,
                     uint32_t *sector, uint32_t *offset);

enum fat_link {
    FAT_LINK_NEXT,          /* `next` is the chain's following cluster */
    FAT_LINK_END,           /* this cluster ends its chain */
    FAT_LINK_FREE,          /* 0: no chain leads through a free cluster */
    FAT_LINK_BAD,           /* the BAD CLUSTER mark */
    FAT_LINK_BROKEN,        /* a cluster number the volume does not have */
};

/* What the entry at `at`, where `fat_entry_place` pointed, says. */
enum fat_link fat_link_at(const struct fat_volume *v, const uint8_t *at,
                          uint32_t *next);

/* A data cluster's first sector. `cluster` is one of the volume's. */
uint32_t fat_cluster_sector(const struct fat_volume *v, uint32_t cluster);

/* FAT16's root directory's first sector; it is `root_sectors` long. */
uint32_t fat_root_sector(const struct fat_volume *v);

/* A long name's pieces, gathered before the short entry they belong to. */
struct fat_names {
    uint16_t units[260];
    uint8_t  checksum;          /* LDIR_Chksum every piece must carry */
    uint8_t  count;             /* how many pieces the set began by saying */
    uint8_t  expect;            /* the ordinal the next piece must have */
    bool     active;
};

void fat_names_clear(struct fat_names *names);

struct fat_dirent {
    char     name[FAT_NAME_BYTES];  /* UTF-8: the long name, else the short in DIR_NTRes's case */
    char     short_name[13];        /* NAME.EXT, as stored */
    uint8_t  attributes;            /* DIR_Attr */
    uint32_t first_cluster;         /* DIR_FstClusHI (FAT32 only) and LO */
    uint32_t size;                  /* DIR_FileSize */
    uint16_t write_time;            /* DIR_WrtTime */
    uint16_t write_date;            /* DIR_WrtDate */
    bool     directory;
    bool     long_name;             /* `name` came from long entries */
};

enum fat_step {
    FAT_STEP_END,           /* 0x00: this entry and every one after it is free */
    FAT_STEP_SKIP,          /* a free entry, dot or dotdot, or one not valid */
    FAT_STEP_PIECE,         /* a long name's piece, kept in `names` */
    FAT_STEP_LABEL,         /* the volume's label, in `out->name` */
    FAT_STEP_ENTRY,         /* a file or a directory, in `out` */
};

/*
 * One 32-byte entry, in the order a directory stores them. `names` carries a
 * long name from its pieces to its short entry, and is the caller's to clear
 * when a directory starts.
 */
enum fat_step fat_dirent_step(const struct fat_volume *v, const uint8_t *entry,
                              struct fat_names *names, struct fat_dirent *out);

/* The checksum a short name's long entries carry: the specification's ChkSum. */
uint8_t fat_short_checksum(const uint8_t *name);

/*
 * Whether `name` is `want`, compared as FAT compares names: without regard to
 * case (Name Matching In Short & Long Names). ASCII letters only - accented
 * letters are compared exactly, which is the one way this is narrower than
 * the specification asks.
 */
bool fat_name_matches(const char *want, const char *name);

#endif /* KOSMOS_SERVERS_FAT_DECODE_H */
