/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where the volumes on a drive are, and what to call them (USB step 6b).
 *
 * `drives_decode.h` says what each of these is for. Nothing here touches
 * hardware or makes a system call: it is handed bytes and answers about them,
 * so `tools/test_drivesdecode.c` can ask the awkward questions on the Mac.
 *
 * Two tables, and they are not the same kind of thing. An MBR is four fixed
 * entries in the drive's first sector, written before anyone expected a drive
 * over 2 TB. A GPT is a header naming an array somewhere else, with a CRC
 * over both. `storage_decode.c` already holds a GPT header to the UEFI
 * specification's checks; this reads the array it points at.
 */

#include "drives_decode.h"
#include "drivers/usb/storage_decode.h"

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
           | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

const char *drives_fs_name(enum drives_fs fs)
{
    switch (fs) {
    case FS_KIND_FAT16: return "FAT16";
    case FS_KIND_FAT32: return "FAT32";
    case FS_KIND_KFS:   return "kfs";
    case FS_KIND_OTHER: return "unknown";
    case FS_KIND_NONE:  break;
    }

    return "none";
}

/*
 * The MBR's signature, which is the same two bytes a FAT boot sector ends
 * with - so this says "there is a table here", never "this is not a FAT
 * volume". A volume with no partition table at all has 0x55 0xAA too, and
 * `tools/test_fat.py` builds exactly that case.
 */
static bool mbr_signed(const uint8_t *sector, unsigned size)
{
    return size >= 512u && sector[510] == 0x55u && sector[511] == 0xAAu;
}

bool mbr_is_protective(const uint8_t *sector, unsigned size)
{
    unsigned i, live = 0, protective = 0;

    if (!mbr_signed(sector, size)) {
        return false;
    }

    for (i = 0; i < MBR_ENTRIES; i++) {
        const uint8_t *e = sector + MBR_TABLE_AT + i * MBR_ENTRY_BYTES;

        if (le32(e + 12) == 0u) {
            continue;           /* no sectors: an empty slot */
        }

        live++;

        if (e[4] == MBR_TYPE_GPT_PROTECTIVE) {
            protective++;
        }
    }

    return live > 0 && live == protective;
}

unsigned mbr_partitions(const uint8_t *sector, unsigned size,
                        struct drives_part *out, unsigned most)
{
    unsigned i, found = 0;

    if (!mbr_signed(sector, size) || out == NULL || most == 0) {
        return 0;
    }

    /*
     * A protective MBR names the whole drive as one 0xEE partition so that a
     * tool which does not know about GPT sees the space as taken. Offering it
     * as a volume would put the entire drive in `/drives` beside the real
     * partitions on it.
     */
    if (mbr_is_protective(sector, size)) {
        return 0;
    }

    for (i = 0; i < MBR_ENTRIES && found < most; i++) {
        const uint8_t *e = sector + MBR_TABLE_AT + i * MBR_ENTRY_BYTES;
        uint32_t first = le32(e + 8);
        uint32_t sectors = le32(e + 12);

        /* An entry of no sectors is an empty slot however its other fields
         * read, which is the one thing every partition table agrees on. */
        if (sectors == 0u) {
            continue;
        }

        out[found].first = first;
        out[found].sectors = sectors;
        out[found].type = e[4];
        out[found].gpt = false;
        out[found].has_guid = false;

        for (unsigned b = 0; b < 16u; b++) {
            out[found].guid[b] = 0u;
        }
        found++;
    }

    return found;
}

bool gpt_entry_array(const uint8_t *header, unsigned size, unsigned most_bytes,
                     uint64_t *at, unsigned *entry_size, unsigned *count)
{
    uint64_t where;
    uint32_t entries, each;

    if (header == NULL || size < GPT_HEADER_LEAST) {
        return false;
    }

    /* PartitionEntryLBA at 72, NumberOfPartitionEntries at 80,
     * SizeOfPartitionEntry at 84 - the same fields `init.lua`'s Lua walk
     * reads at 73, 81 and 85, one-based. */
    where = le64(header + 72);
    entries = le32(header + 80);
    each = le32(header + 84);

    /*
     * Held to something sane before it becomes a length to read. A header is
     * bytes from a drive somebody else formatted, and every one of these is
     * used to size a read: 128 is the specification's minimum entry, and an
     * array larger than the caller will read in one go is refused rather than
     * half-read, because half an array is a drive with partitions missing and
     * nothing to say so.
     */
    if (where == 0u || entries == 0u || each < 128u || each > 4096u) {
        return false;
    }

    if ((uint64_t)entries * (uint64_t)each > (uint64_t)most_bytes) {
        return false;
    }

    if (at != NULL) {
        *at = where;
    }

    if (entry_size != NULL) {
        *entry_size = each;
    }

    if (count != NULL) {
        *count = entries;
    }

    return true;
}

/* An entry whose type GUID is sixteen zero bytes is unused (UEFI 5.3.3). */
static bool gpt_entry_used(const uint8_t *e)
{
    unsigned i;

    for (i = 0; i < 16u; i++) {
        if (e[i] != 0u) {
            return true;
        }
    }

    return false;
}

unsigned gpt_partitions(const uint8_t *entries, unsigned bytes,
                        unsigned entry_size, unsigned count,
                        struct drives_part *out, unsigned most)
{
    unsigned i, found = 0;

    if (entries == NULL || out == NULL || most == 0
        || entry_size < 128u || count == 0u) {
        return 0;
    }

    for (i = 0; i < count && found < most; i++) {
        const uint8_t *e = entries + (uint64_t)i * entry_size;
        uint64_t first, last;

        /* Only as far as the bytes really given, whatever the header claimed
         * the array holds. */
        if ((uint64_t)(i + 1u) * entry_size > (uint64_t)bytes) {
            break;
        }

        if (!gpt_entry_used(e)) {
            continue;
        }

        /* StartingLBA at 32, EndingLBA at 40, and the end is inclusive. */
        first = le64(e + 32);
        last = le64(e + 40);

        if (last < first) {
            continue;           /* a backwards partition is not one */
        }

        out[found].first = first;
        out[found].sectors = last - first + 1u;
        out[found].type = 0u;
        out[found].gpt = true;

        /* UniquePartitionGUID at 16, beside the type GUID (UEFI 5.3.3). */
        for (unsigned b = 0; b < 16u; b++) {
            out[found].guid[b] = e[16 + b];
        }

        out[found].has_guid = true;
        found++;
    }

    return found;
}

bool kfs_super_from(const uint8_t *block, unsigned size,
                    struct kfs_super *out, const char **why)
{
    struct kfs_super s;

    if (why != NULL) {
        *why = "";
    }

    /* Ten 32-bit words and a 64-bit time: 48 bytes, of a 4096-byte block. */
    if (block == NULL || size < 48u) {
        if (why != NULL) {
            *why = "the superblock is short";
        }

        return false;
    }

    s.magic = le32(block);
    s.version = le32(block + 4);
    s.block_size = le32(block + 8);
    s.blocks = le32(block + 12);
    s.bitmap_at = le32(block + 16);
    s.bitmap_blocks = le32(block + 20);
    s.inodes_at = le32(block + 24);
    s.inode_count = le32(block + 28);
    s.journal_at = le32(block + 32);
    s.data_at = le32(block + 36);

    if (s.magic != KFS_MAGIC) {
        if (why != NULL) {
            *why = "not a kosmos filesystem";
        }

        return false;
    }

    if (s.version != KFS_VERSION) {
        if (why != NULL) {
            *why = "a version this does not understand";
        }

        return false;
    }

    if (s.block_size != KFS_BLOCK) {
        if (why != NULL) {
            *why = "blocks of a size this does not understand";
        }

        return false;
    }

    /*
     * The same layout check `kfs.unpack_super` makes, and for the same
     * reason: every one of these is used as an offset into the volume, and a
     * plausible wrong number is how something that was fine gets corrupted.
     * Read-only here, so the cost is a wrong answer rather than a wrong
     * write - which is still worth refusing.
     */
    if (s.data_at <= s.inodes_at || s.inodes_at <= s.bitmap_at
        || s.bitmap_at == 0u || s.data_at >= s.blocks) {
        if (why != NULL) {
            *why = "the superblock's layout does not make sense";
        }

        return false;
    }

    if (out != NULL) {
        *out = s;
    }

    return true;
}

uint32_t kfs_free_in(const uint8_t *bytes, unsigned n, uint32_t already,
                     uint32_t cap)
{
    uint32_t free = already;
    unsigned i;

    if (bytes == NULL) {
        return free;
    }

    for (i = 0; i < n && free < cap; i++) {
        uint8_t b = bytes[i];
        unsigned bit;

        for (bit = 0; bit < 8u && free < cap; bit++) {
            if ((b & (1u << bit)) == 0u) {
                free++;
            }
        }
    }

    return free;
}

uint32_t fat_fsinfo_sector(const uint8_t *boot, unsigned size,
                           const struct fat_volume *v)
{
    /* BPB_FSInfo is a FAT32 field and sits at byte 48; FAT16's boot sector
     * has other things there, so the kind decides whether to look. */
    if (boot == NULL || size < 512u || v == NULL || v->kind != FAT_32) {
        return 0u;
    }

    return le16(boot + 48);
}

bool fat_fsinfo_from(const uint8_t *sector, unsigned size,
                     struct fat_fsinfo *out)
{
    uint32_t free_count;

    if (sector == NULL || size < 512u) {
        return false;
    }

    /* All three signatures, because two of them sit in a sector that is
     * otherwise reserved and zero - so one alone would match a blank. */
    if (le32(sector) != FAT_FSINFO_LEAD
        || le32(sector + 484) != FAT_FSINFO_STRUCT
        || le32(sector + 508) != FAT_FSINFO_TRAIL) {
        return false;
    }

    free_count = le32(sector + 488);

    if (out != NULL) {
        out->free_clusters = free_count;
        out->next_free = le32(sector + 492);
        out->free_known = free_count != FAT_FSINFO_UNKNOWN;
    }

    return true;
}

/*
 * A label as a name. Printable ASCII only, and no `/`.
 *
 * FAT stores a label as eleven bytes in whatever code page wrote it, and
 * which one that was is not recorded - the same reason `fat_decode.c` shows a
 * short name's non-ASCII bytes as `_`. A name is also a path component here,
 * so a `/` in one would silently become a directory nobody can reach.
 */
void drives_label_name(char *out, unsigned bytes, const char *label)
{
    unsigned at = 0;

    if (out == NULL || bytes == 0) {
        return;
    }

    if (label != NULL) {
        unsigned i;

        for (i = 0; label[i] != '\0' && at + 1u < bytes; i++) {
            unsigned char c = (unsigned char)label[i];

            if (c == '/' || c < 0x20u || c > 0x7Eu) {
                c = '_';
            }

            out[at++] = (char)c;
        }

        /* Trailing spaces are padding rather than a name: FAT pads a label
         * with them, and `Untitled` is what a label of nothing but padding
         * means. */
        while (at > 0 && out[at - 1u] == ' ') {
            at--;
        }
    }

    if (at == 0) {
        const char *none = "Untitled";
        unsigned i;

        for (i = 0; none[i] != '\0' && i + 1u < bytes; i++) {
            out[i] = none[i];
        }

        at = i;
    }

    out[at] = '\0';
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Without regard to case, as FAT compares names - so two drives differing
 * only in case are one name and get numbered apart. */
static bool same_name(const char *a, const char *b)
{
    unsigned i;

    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }

    return a[i] == b[i];
}

static bool already_taken(const char *name, const char *const *taken,
                          unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        if (taken[i] != NULL && same_name(name, taken[i])) {
            return true;
        }
    }

    return false;
}

void drives_unique_name(char *name, unsigned bytes,
                        const char *const *taken, unsigned count)
{
    char base[DRIVES_NAME_BYTES];
    unsigned n, at;

    if (name == NULL || bytes == 0 || taken == NULL) {
        return;
    }

    if (!already_taken(name, taken, count)) {
        return;
    }

    for (at = 0; at + 1u < bytes && at + 1u < sizeof(base)
                 && name[at] != '\0'; at++) {
        base[at] = name[at];
    }

    base[at] = '\0';

    /*
     * From two, because the first one to arrive keeps the bare name. The
     * ceiling is the number of volumes that can exist, and it is here so that
     * a caller passing a `taken` list which somehow holds every number does
     * not spin: the last one wins the name rather than the loop never ending.
     */
    for (n = 2u; n < 1000u; n++) {
        unsigned i = 0, d;
        char digits[4];
        unsigned k = 0;
        uint32_t left = n;

        while (left > 0u && k < sizeof(digits)) {
            digits[k++] = (char)('0' + (left % 10u));
            left /= 10u;
        }

        for (i = 0; base[i] != '\0' && i + 1u < bytes; i++) {
            name[i] = base[i];
        }

        if (i + 2u < bytes) {
            name[i++] = ' ';

            for (d = k; d > 0u && i + 1u < bytes; d--) {
                name[i++] = digits[d - 1u];
            }
        }

        name[i] = '\0';

        if (!already_taken(name, taken, count)) {
            return;
        }
    }
}
