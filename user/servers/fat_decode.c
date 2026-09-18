/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * FAT's on-disk format, read. `fat_decode.h` says what for. Each rule names the
 * section of Microsoft's FAT32 File System Specification 1.03 it comes from,
 * so a reader can hold this file to that one.
 */

#include <string.h>

#include "fat_decode.h"

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool refused(const char **why, const char *text)
{
    if (why != NULL) {
        *why = text;
    }

    return false;
}

const char *fat_kind_name(enum fat_kind kind)
{
    switch (kind) {
    case FAT_12: return "FAT12";
    case FAT_16: return "FAT16";
    case FAT_32: return "FAT32";
    default:     return "not FAT";
    }
}

/*
 * A byte of a short name or a label, as a character. Short names are in
 * whichever OEM code page made them, which nothing here knows, and a
 * character that cannot be translated "is always translated to the _
 * (underscore) character" (Name Matching In Short & Long Names). Below 0x20 is
 * not legal in a short name at all, and becomes the same.
 */
static char shown(uint8_t c)
{
    return (c < 0x20u || c >= 0x80u) ? '_' : (char)c;
}

/* `n` bytes of space-padded name, trailing spaces dropped. Returns the length. */
static unsigned trimmed(const uint8_t *from, unsigned n, char *to)
{
    unsigned length = n;

    while (length > 0u && from[length - 1u] == ' ') {
        length--;
    }

    for (unsigned i = 0; i < length; i++) {
        to[i] = shown(from[i]);
    }

    to[length] = '\0';
    return length;
}

bool fat_volume_from(const uint8_t *s, unsigned size, struct fat_volume *out,
                     const char **why)
{
    uint32_t total16, total32, fat16, fat32;
    uint64_t metadata;

    memset(out, 0, sizeof(*out));
    out->kind = FAT_NONE;

    if (size < 512u) {
        return refused(why, "a boot sector is 512 bytes");
    }

    /* "sector[510] equals 0x55, and sector[511] equals 0xAA" - Boot Sector and BPB. */
    if (s[510] != 0x55u || s[511] != 0xAAu) {
        return refused(why, "no boot sector signature at bytes 510 and 511");
    }

    /* BS_jmpBoot: EB ?? 90, or E9 ?? ??. */
    if (!((s[0] == 0xEBu && s[2] == 0x90u) || s[0] == 0xE9u)) {
        return refused(why, "no jump at byte 0, so not a FAT boot sector");
    }

    out->bytes_per_sector = le16(s + 11);

    if (out->bytes_per_sector != 512u && out->bytes_per_sector != 1024u
        && out->bytes_per_sector != 2048u && out->bytes_per_sector != 4096u) {
        return refused(why, "BPB_BytsPerSec is not 512, 1024, 2048 or 4096");
    }

    out->sectors_per_cluster = s[13];

    if (out->sectors_per_cluster == 0u
        || (out->sectors_per_cluster & (out->sectors_per_cluster - 1u)) != 0u) {
        return refused(why, "BPB_SecPerClus is not a power of two");
    }

    /*
     * The specification says clusters over 32 KB "do not work properly", and
     * that some systems made 64 KB ones anyway. Reading one costs nothing.
     */
    out->cluster_bytes = out->bytes_per_sector * out->sectors_per_cluster;

    if (out->cluster_bytes > 65536u) {
        return refused(why, "a cluster over 64 KB");
    }

    out->reserved_sectors = le16(s + 14);
    out->fats = s[16];
    out->root_entries = le16(s + 17);
    total16 = le16(s + 19);
    fat16 = le16(s + 22);
    total32 = le32(s + 32);

    if (out->reserved_sectors == 0u) {
        return refused(why, "BPB_RsvdSecCnt is 0");
    }

    if (out->fats == 0u) {
        return refused(why, "BPB_NumFATs is 0");
    }

    out->total_sectors = (total16 != 0u) ? total16 : total32;

    if (out->total_sectors == 0u) {
        return refused(why, "no sector count in BPB_TotSec16 or BPB_TotSec32");
    }

    /* BPB_FATSz32 exists only when BPB_FATSz16 is 0; before that, offset 36 is FAT16's BS_DrvNum. */
    fat32 = (fat16 == 0u) ? le32(s + 36) : 0u;
    out->fat_sectors = (fat16 != 0u) ? fat16 : fat32;

    if (out->fat_sectors == 0u) {
        return refused(why, "no FAT size in BPB_FATSz16 or BPB_FATSz32");
    }

    /* RootDirSectors, rounded up - FAT Data Structure. */
    out->root_sectors = (out->root_entries * 32u + out->bytes_per_sector - 1u)
                      / out->bytes_per_sector;

    metadata = (uint64_t)out->reserved_sectors
             + (uint64_t)out->fats * out->fat_sectors + out->root_sectors;

    if (metadata >= out->total_sectors) {
        return refused(why, "the FATs and the root directory leave no data region");
    }

    out->first_data_sector = (uint32_t)metadata;

    /* CountofClusters, rounded down - FAT Type Determination. */
    out->clusters = (out->total_sectors - out->first_data_sector)
                  / out->sectors_per_cluster;

    /*
     * "The FAT type ... is determined by the count of clusters on the volume
     * and nothing else", and "when it says <, it does not mean <=".
     */
    if (out->clusters < 4085u) {
        out->kind = FAT_12;
        return refused(why, "FAT12, which Kosmos does not read");
    }

    out->kind = (out->clusters < 65525u) ? FAT_16 : FAT_32;

    if (out->kind == FAT_16) {
        if (fat16 == 0u) {
            return refused(why, "a FAT16 volume whose BPB_FATSz16 is 0");
        }

        if (out->root_entries == 0u) {
            return refused(why, "a FAT16 volume with no root directory entries");
        }

        /* Two bytes an entry, for every cluster and the two reserved ones. */
        if ((uint64_t)out->fat_sectors * out->bytes_per_sector
            < ((uint64_t)out->clusters + 2u) * 2u) {
            return refused(why, "a FAT too small for the volume's clusters");
        }

        /* BS_BootSig 0x29 says BS_VolLab is there. */
        if (s[38] == 0x29u) {
            trimmed(s + 43, 11u, out->label);
            /* BS_VolID, just before the label: the volume's own serial. */
            out->serial = le32(s + 39);
            out->has_serial = true;
        }
    } else {
        /* "For FAT32 volumes, this field must be" 0: BPB_RootEntCnt, BPB_TotSec16, BPB_FATSz16. */
        if (out->root_entries != 0u || total16 != 0u || fat16 != 0u) {
            return refused(why, "a FAT32 volume with FAT16's fields set");
        }

        /*
         * BPB_FSVer: drivers "must check this field and not mount the volume
         * if it does not contain a version number that was defined at the
         * time the driver was written", which is 0:0.
         */
        if (le16(s + 42) != 0u) {
            return refused(why, "a FAT32 version newer than 0.0");
        }

        out->root_cluster = le32(s + 44);

        if (out->root_cluster < 2u || out->root_cluster > out->clusters + 1u) {
            return refused(why, "BPB_RootClus is not one of the volume's clusters");
        }

        if ((uint64_t)out->fat_sectors * out->bytes_per_sector
            < ((uint64_t)out->clusters + 2u) * 4u) {
            return refused(why, "a FAT too small for the volume's clusters");
        }

        if (s[66] == 0x29u) {
            trimmed(s + 71, 11u, out->label);
            /* BS_VolID, just before the label: the volume's own serial. */
            out->serial = le32(s + 67);
            out->has_serial = true;
        }
    }

    /* "The setting for this field when there is no volume label is ... NO NAME". */
    if (strcmp(out->label, "NO NAME") == 0) {
        out->label[0] = '\0';
    }

    return true;
}

bool fat_entry_place(const struct fat_volume *v, uint32_t cluster,
                     uint32_t *sector, uint32_t *offset)
{
    uint64_t at;

    /* "The first data cluster is cluster 2", and the last is CountofClusters + 1. */
    if (cluster < 2u || cluster > v->clusters + 1u) {
        return false;
    }

    /* FATOffset, ThisFATSecNum and ThisFATEntOffset - FAT Type Determination. */
    at = (uint64_t)cluster * ((v->kind == FAT_32) ? 4u : 2u);
    *sector = v->reserved_sectors + (uint32_t)(at / v->bytes_per_sector);
    *offset = (uint32_t)(at % v->bytes_per_sector);
    return true;
}

enum fat_link fat_link_at(const struct fat_volume *v, const uint8_t *at,
                          uint32_t *next)
{
    uint32_t value;

    if (v->kind == FAT_32) {
        /* "A FAT32 FAT entry is actually only a 28-bit entry." */
        value = le32(at) & 0x0FFFFFFFu;

        if (value >= 0x0FFFFFF8u) {
            return FAT_LINK_END;
        }

        /* "no FAT32 volume should ever be configured such that 0x0FFFFFF7 is an allocatable cluster number" */
        if (value == 0x0FFFFFF7u) {
            return FAT_LINK_BAD;
        }
    } else {
        value = le16(at);

        if (value >= 0xFFF8u) {
            return FAT_LINK_END;
        }

        if (value == 0xFFF7u) {
            return FAT_LINK_BAD;
        }
    }

    if (value == 0u) {
        return FAT_LINK_FREE;
    }

    if (value < 2u || value > v->clusters + 1u) {
        return FAT_LINK_BROKEN;
    }

    *next = value;
    return FAT_LINK_NEXT;
}

uint32_t fat_cluster_sector(const struct fat_volume *v, uint32_t cluster)
{
    /* FirstSectorofCluster - FAT Data Structure. */
    return (cluster - 2u) * v->sectors_per_cluster + v->first_data_sector;
}

uint32_t fat_root_sector(const struct fat_volume *v)
{
    /* FirstRootDirSecNum - FAT Directory Structure. */
    return v->reserved_sectors + v->fats * v->fat_sectors;
}

void fat_names_clear(struct fat_names *names)
{
    memset(names, 0, sizeof(*names));
}

uint8_t fat_short_checksum(const uint8_t *name)
{
    uint8_t sum = 0;

    /* ChkSum, "an unsigned char rotate right" - Organization and Association of Short & Long Directory Entries. */
    for (unsigned i = 0; i < 11u; i++) {
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + name[i]);
    }

    return sum;
}

/* One code point as UTF-8 at `at`, if it fits before `end`. Returns the new place. */
static unsigned put_utf8(char *to, unsigned at, unsigned end, uint32_t c)
{
    if (c < 0x80u) {
        if (at + 1u > end) return at;
        to[at++] = (char)c;
    } else if (c < 0x800u) {
        if (at + 2u > end) return at;
        to[at++] = (char)(0xC0u | (c >> 6));
        to[at++] = (char)(0x80u | (c & 0x3Fu));
    } else if (c < 0x10000u) {
        if (at + 3u > end) return at;
        to[at++] = (char)(0xE0u | (c >> 12));
        to[at++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
        to[at++] = (char)(0x80u | (c & 0x3Fu));
    } else {
        if (at + 4u > end) return at;
        to[at++] = (char)(0xF0u | (c >> 18));
        to[at++] = (char)(0x80u | ((c >> 12) & 0x3Fu));
        to[at++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
        to[at++] = (char)(0x80u | (c & 0x3Fu));
    }

    return at;
}

/*
 * The long name the pieces spell, as UTF-8. "Long names are stored in long
 * directory entries in UNICODE", "NUL terminated and padded with 0xFFFF" unless
 * they fill their entries exactly. A surrogate without its pair cannot be
 * translated, and is shown as `_` for the reason `shown` gives.
 */
static unsigned long_name(const struct fat_names *names, char *to)
{
    unsigned n = names->count * 13u, at = 0, end = FAT_NAME_BYTES - 1u;

    for (unsigned i = 0; i < n; i++) {
        uint32_t c = names->units[i];

        if (c == 0u) {
            break;
        }

        if (c >= 0xD800u && c < 0xDC00u && i + 1u < n
            && names->units[i + 1u] >= 0xDC00u && names->units[i + 1u] < 0xE000u) {
            c = 0x10000u + ((c - 0xD800u) << 10) + (names->units[i + 1u] - 0xDC00u);
            i++;
        } else if (c >= 0xD800u && c < 0xE000u) {
            c = '_';
        }

        at = put_utf8(to, at, end, c);
    }

    to[at] = '\0';
    return at;
}

enum fat_step fat_dirent_step(const struct fat_volume *v, const uint8_t *e,
                              struct fat_names *names, struct fat_dirent *out)
{
    uint8_t attr = e[11];
    uint32_t kind;
    unsigned length, base_length;

    /* DIR_Name[0] - FAT Directory Structure. */
    if (e[0] == 0x00u) {
        fat_names_clear(names);
        return FAT_STEP_END;
    }

    if (e[0] == 0xE5u) {
        fat_names_clear(names);
        return FAT_STEP_SKIP;
    }

    if ((attr & FAT_ATTR_LONG_NAME_MASK) == FAT_ATTR_LONG_NAME) {
        uint8_t ordinal = e[0] & 0x3Fu;
        unsigned first;

        /*
         * LDIR_Type zero marks a long name's piece: "Non-zero implies other
         * dirent types", which are not names. LDIR_FstClusLO "Must be ZERO",
         * and is not held to it: "The 'first cluster' field is currently being
         * set to zero, though this might change in future" (Validating The
         * Contents of a Directory).
         */
        if (e[12] != 0u) {
            fat_names_clear(names);
            return FAT_STEP_SKIP;
        }

        if ((e[0] & 0x40u) != 0u) {
            /* LAST_LONG_ENTRY: "All valid sets of long dir entries must begin with an entry having this mask." */
            fat_names_clear(names);

            if (ordinal == 0u || ordinal > 20u) {
                return FAT_STEP_SKIP;
            }

            names->active = true;
            names->count = ordinal;
            names->expect = ordinal;
            names->checksum = e[13];
        } else if (!names->active || ordinal != names->expect
                   || e[13] != names->checksum) {
            /* Out of order, or another name's checksum: "treated as orphans". */
            fat_names_clear(names);
            return FAT_STEP_SKIP;
        }

        /* Characters 1-5, 6-11 and 12-13 of this piece, which is piece `expect`. */
        first = (unsigned)(names->expect - 1u) * 13u;

        for (unsigned i = 0; i < 5u; i++) {
            names->units[first + i] = (uint16_t)le16(e + 1 + 2 * i);
        }

        for (unsigned i = 0; i < 6u; i++) {
            names->units[first + 5u + i] = (uint16_t)le16(e + 14 + 2 * i);
        }

        for (unsigned i = 0; i < 2u; i++) {
            names->units[first + 11u + i] = (uint16_t)le16(e + 28 + 2 * i);
        }

        names->expect--;
        return FAT_STEP_PIECE;
    }

    memset(out, 0, sizeof(*out));

    kind = attr & (FAT_ATTR_DIRECTORY | FAT_ATTR_VOLUME_ID);

    if (kind == FAT_ATTR_VOLUME_ID) {
        /* The label, which "must be in the root directory". */
        trimmed(e, 11u, out->name);
        memcpy(out->short_name, out->name, sizeof(out->short_name) - 1u);
        fat_names_clear(names);
        return FAT_STEP_LABEL;
    }

    if (kind != 0u && kind != FAT_ATTR_DIRECTORY) {
        /* Both a directory and a label: "an invalid directory entry". */
        fat_names_clear(names);
        return FAT_STEP_SKIP;
    }

    /* The dot and dotdot entries every directory but the root begins with. */
    if (memcmp(e, ".          ", 11u) == 0 || memcmp(e, "..         ", 11u) == 0) {
        fat_names_clear(names);
        return FAT_STEP_SKIP;
    }

    /*
     * "If DIR_Name[0] == 0x05, then the actual file name character for this
     * byte is 0xE5." What that rule changes here is only that such an entry is
     * not free, which the test above already says: 0xE5 is a code page
     * character, every byte outside ASCII is shown as '_', and so is 0x05.
     * Short names read through a code page would have to turn 0x05 into 0xE5
     * first.
     */
    length = trimmed(e, 8u, out->short_name);

    base_length = length;

    if (e[8] != ' ' || e[9] != ' ' || e[10] != ' ') {
        out->short_name[length++] = '.';
        trimmed(e + 8, 3u, out->short_name + length);
    }

    out->attributes = attr;
    out->directory = (attr & FAT_ATTR_DIRECTORY) != 0u;
    out->size = le32(e + 28);
    out->write_time = (uint16_t)le16(e + 22);
    out->write_date = (uint16_t)le16(e + 24);

    /* DIR_FstClusHI is "always 0 for a FAT12 or FAT16 volume". */
    out->first_cluster = le16(e + 26);

    if (v->kind == FAT_32) {
        out->first_cluster |= le16(e + 20) << 16;
    }

    /* A long name counts only when every piece came, in order, for this short name. */
    if (names->active && names->expect == 0u
        && names->checksum == fat_short_checksum(e)
        && long_name(names, out->name) > 0u) {
        out->long_name = true;
    } else {
        memcpy(out->name, out->short_name, sizeof(out->short_name));

        /*
         * DIR_NTRes, which the specification reserves "for use by Windows
         * NT". Windows NT uses bit 3 for a base name in lower case and bit 4
         * for an extension in lower case, so that hello.txt needs no long
         * entries; mtools writes the same two bits, and `tools/test_fat.py`
         * holds this to what it wrote. Only the name shown changes -
         * `short_name` stays as stored.
         */
        for (unsigned i = 0; out->name[i] != '\0'; i++) {
            uint8_t bit = (i < base_length) ? 0x08u : 0x10u;

            if ((e[12] & bit) != 0u && out->name[i] >= 'A' && out->name[i] <= 'Z') {
                out->name[i] = (char)(out->name[i] - 'A' + 'a');
            }
        }
    }

    fat_names_clear(names);
    return FAT_STEP_ENTRY;
}

bool fat_name_matches(const char *want, const char *name)
{
    for (;; want++, name++) {
        char a = *want, b = *name;

        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');

        if (a != b) {
            return false;
        }

        if (a == '\0') {
            return true;
        }
    }
}
