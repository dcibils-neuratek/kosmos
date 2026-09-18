/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `drives_decode.c` on this machine: where the volumes on a drive are, and
 * what they are called (USB step 6b).
 *
 * The bytes are built here, from what each table's specification says one
 * looks like, so the awkward cases can be asked about a byte at a time -
 * `fat_decode.c`'s arrangement and `storage_decode.c`'s before it.
 *
 * **What this is watching for is the cases a real drive rarely shows.** A
 * table read correctly on Diego's Kingston says nothing about a protective
 * MBR, an entry whose type byte is set and whose sector count is zero, a GPT
 * entry that ends before it starts, or a label of nothing but padding. Those
 * are where a reader either refuses or quietly offers a volume that is not
 * there.
 */

#include <stdio.h>
#include <string.h>

#include "../user/servers/drives_decode.h"

/*
 * A kfs volume `mkfs` really wrote, made by the Makefile rule beside this
 * test with the same `tools/kfs.lua` the machine itself runs.
 */
#ifndef KFS_FIXTURE
#define KFS_FIXTURE "build/host/kfs-fixture.img"
#endif

static int checks;
static int failures;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static void put64(unsigned char *p, uint64_t v)
{
    put32(p, (uint32_t)(v & 0xFFFFFFFFu));
    put32(p + 4, (uint32_t)(v >> 32));
}

/* One MBR entry: its type at +4, first sector at +8, count at +12 - the
 * layout `tools/test_fat.py` already writes to. */
static void mbr_entry(unsigned char *sector, unsigned slot, unsigned char type,
                      uint32_t first, uint32_t sectors)
{
    unsigned char *e = sector + MBR_TABLE_AT + slot * MBR_ENTRY_BYTES;

    e[4] = type;
    put32(e + 8, first);
    put32(e + 12, sectors);
}

static void mbr_sign(unsigned char *sector)
{
    sector[510] = 0x55u;
    sector[511] = 0xAAu;
}

static void test_mbr(void)
{
    unsigned char s[512];
    struct drives_part parts[DRIVES_PARTS_MAX];
    unsigned n;

    memset(s, 0, sizeof(s));
    mbr_sign(s);
    mbr_entry(s, 0, MBR_TYPE_FAT32_LBA, 2048u, 100000u);
    mbr_entry(s, 1, MBR_TYPE_FAT16_LBA, 200000u, 50000u);

    n = mbr_partitions(s, sizeof(s), parts, DRIVES_PARTS_MAX);
    check(n == 2, "two MBR partitions are found");
    check(n == 2 && parts[0].first == 2048u && parts[0].sectors == 100000u,
          "the first partition's place is read");
    check(n == 2 && !parts[0].has_guid && !parts[1].has_guid,
          "an MBR partition has no GUID to be remembered by");
    check(n == 2 && parts[0].type == MBR_TYPE_FAT32_LBA && !parts[0].gpt,
          "its type byte is kept and it is not marked GPT");
    check(n == 2 && parts[1].first == 200000u,
          "the second partition is read past the first");

    /* No signature: not a table at all. */
    memset(s, 0, sizeof(s));
    mbr_entry(s, 0, MBR_TYPE_FAT32_LBA, 2048u, 100000u);
    check(mbr_partitions(s, sizeof(s), parts, DRIVES_PARTS_MAX) == 0,
          "a sector with no 0x55 0xAA holds no partitions");

    /*
     * A type byte with no sectors is an empty slot. Formatting tools leave
     * these behind, and a reader that trusted the type would offer a volume
     * at sector zero of length zero.
     */
    memset(s, 0, sizeof(s));
    mbr_sign(s);
    mbr_entry(s, 0, MBR_TYPE_FAT32_LBA, 0u, 0u);
    mbr_entry(s, 2, MBR_TYPE_FAT16_LBA, 4096u, 1000u);
    n = mbr_partitions(s, sizeof(s), parts, DRIVES_PARTS_MAX);
    check(n == 1 && parts[0].first == 4096u,
          "an entry of no sectors is an empty slot however its type reads");

    /* A protective MBR is the whole drive as 0xEE, and is not a volume. */
    memset(s, 0, sizeof(s));
    mbr_sign(s);
    mbr_entry(s, 0, MBR_TYPE_GPT_PROTECTIVE, 1u, 0xFFFFFFFFu);
    check(mbr_is_protective(s, sizeof(s)),
          "a lone 0xEE entry is a protective MBR");
    check(mbr_partitions(s, sizeof(s), parts, DRIVES_PARTS_MAX) == 0,
          "a protective MBR offers no partitions of its own");

    /* 0xEE beside a real partition is not protective - that is a drive
     * somebody has half-converted, and the real one still counts. */
    mbr_entry(s, 1, MBR_TYPE_FAT32_LBA, 2048u, 1000u);
    check(!mbr_is_protective(s, sizeof(s)),
          "0xEE beside a real partition is not a protective MBR");
    check(mbr_partitions(s, sizeof(s), parts, DRIVES_PARTS_MAX) == 2,
          "and both of its entries are offered");

    /* An empty table is signed and holds nothing. */
    memset(s, 0, sizeof(s));
    mbr_sign(s);
    check(mbr_partitions(s, sizeof(s), parts, DRIVES_PARTS_MAX) == 0,
          "a signed but empty table holds no partitions");
    check(!mbr_is_protective(s, sizeof(s)),
          "an empty table is not protective either");
}

static const unsigned char unique_guid[16] = {
    0x95, 0x1D, 0x23, 0xBA, 0x76, 0x95, 0x49, 0x43,
    0xA3, 0x59, 0x1D, 0x3F, 0xD2, 0xB0, 0x45, 0xD8,
};

static void test_gpt(void)
{
    unsigned char header[512];
    unsigned char entries[512];
    struct drives_part parts[DRIVES_PARTS_MAX];
    uint64_t at = 0;
    unsigned each = 0, count = 0, n;

    memset(header, 0, sizeof(header));
    memcpy(header, "EFI PART", 8);
    put64(header + 72, 2u);             /* PartitionEntryLBA */
    put32(header + 80, 4u);             /* NumberOfPartitionEntries */
    put32(header + 84, 128u);           /* SizeOfPartitionEntry */

    check(gpt_entry_array(header, sizeof(header), 65536u, &at, &each, &count),
          "a GPT header names its entry array");
    check(at == 2u && each == 128u && count == 4u,
          "the array's place, entry size and count are read");

    /* An array larger than the caller will read is refused whole, rather
     * than half-read into a drive with partitions missing. */
    put32(header + 80, 1000000u);
    check(!gpt_entry_array(header, sizeof(header), 65536u, &at, &each, &count),
          "an entry array too large to read in one go is refused");
    put32(header + 80, 4u);

    /* An entry smaller than the specification's minimum is not one. */
    put32(header + 84, 64u);
    check(!gpt_entry_array(header, sizeof(header), 65536u, &at, &each, &count),
          "an entry size below 128 bytes is refused");
    put32(header + 84, 128u);

    memset(entries, 0, sizeof(entries));

    /* Entry 0: used, sectors 2048 to 4095 inclusive, and a unique GUID of
     * sixteen different bytes, so one read from the wrong offset cannot
     * match it by coincidence. */
    entries[0] = 0xA2u;                 /* a non-zero type GUID */
    memcpy(entries + 16, unique_guid, sizeof(unique_guid));
    put64(entries + 32, 2048u);
    put64(entries + 40, 4095u);

    /* Entry 1: unused - an all-zero type GUID - but with a plausible range,
     * which is what an entry array full of blanks looks like. */
    put64(entries + 128 + 32, 9000u);
    put64(entries + 128 + 40, 9999u);

    /* Entry 2: used, and ending before it starts. */
    entries[256] = 0xA2u;
    put64(entries + 256 + 32, 5000u);
    put64(entries + 256 + 40, 4000u);

    /* Entry 3: used, one sector long - the smallest a partition can be. */
    entries[384] = 0xA2u;
    put64(entries + 384 + 32, 7000u);
    put64(entries + 384 + 40, 7000u);

    n = gpt_partitions(entries, sizeof(entries), 128u, 4u, parts,
                       DRIVES_PARTS_MAX);
    check(n == 2, "an unused entry and a backwards one are both skipped");
    check(n == 2 && parts[0].first == 2048u && parts[0].sectors == 2048u,
          "a GPT partition's length counts its last sector");
    check(n == 2 && parts[0].gpt && parts[0].type == 0u,
          "a GPT partition is marked as one and carries no MBR type");
    check(n == 2 && parts[1].first == 7000u && parts[1].sectors == 1u,
          "a partition of one sector is one sector long");
    check(n == 2 && parts[0].has_guid
          && memcmp(parts[0].guid, unique_guid, 16) == 0,
          "a GPT partition carries its UniquePartitionGUID, bytes 16 to 31");

    /* The header may claim more entries than the bytes really given. */
    n = gpt_partitions(entries, 256u, 128u, 4u, parts, DRIVES_PARTS_MAX);
    check(n == 1,
          "only the entries the bytes actually hold are read, whatever the "
          "header claimed");
}

static void test_kfs(void)
{
    unsigned char block[4096];
    struct kfs_super sb;
    const char *why = NULL;
    unsigned char bitmap[8];

    /*
     * **A superblock `mkfs` really wrote**, not one built here.
     *
     * This test first hand-assembled one from the constants in
     * `drives_decode.h`, and it passed while agreeing with nothing: a 32 MB
     * volume's real `inode_count` is 512, its journal starts at 18 and its
     * data at 274, where the invented numbers were 64, 40 and 64. The layout
     * check accepts both, so the mistake was invisible - and the C header is
     * the *second* copy of a layout whose authority is `string.pack` in
     * `user/lib/kfs.lua`, which is exactly the arrangement that needs a
     * witness rather than a comment.
     *
     * `build/host/kfs-fixture.img` is made by the Makefile rule beside this
     * test, with the same `tools/kfs.lua` the machine itself runs.
     */
    {
        FILE *f = fopen(KFS_FIXTURE, "rb");
        size_t got = 0;

        if (f != NULL) {
            got = fread(block, 1u, sizeof(block), f);
            fclose(f);
        }

        check(got == sizeof(block),
              "the kfs fixture volume was there to read");
    }

    check(kfs_super_from(block, sizeof(block), &sb, &why),
          "a superblock mkfs wrote is recognised");
    check(sb.block_size == KFS_BLOCK && sb.version == KFS_VERSION,
          "the C header's block size and version are the ones mkfs used");
    check(sb.bitmap_at > 0u && sb.inodes_at > sb.bitmap_at
          && sb.data_at > sb.inodes_at && sb.data_at < sb.blocks,
          "and its regions are in the order the layout check requires");

    put32(block, 0x12345678u);
    check(!kfs_super_from(block, sizeof(block), &sb, &why)
          && strcmp(why, "not a kosmos filesystem") == 0,
          "a block with the wrong magic is refused, and says so");
    put32(block, KFS_MAGIC);

    put32(block + 4, 99u);
    check(!kfs_super_from(block, sizeof(block), &sb, &why),
          "a version this does not understand is refused");
    put32(block + 4, KFS_VERSION);

    /* Regions out of order: the check that stops a plausible wrong number
     * being used as an offset. */
    put32(block + 36, 1u);              /* data_at below inodes_at */
    check(!kfs_super_from(block, sizeof(block), &sb, &why),
          "a superblock whose regions are out of order is refused");
    put32(block + 36, 64u);

    put32(block + 36, 9000u);           /* data_at past the volume's end */
    check(!kfs_super_from(block, sizeof(block), &sb, &why),
          "a superblock whose data starts past its end is refused");
    put32(block + 36, 64u);

    check(!kfs_super_from(block, 8u, &sb, &why),
          "a short superblock is refused rather than read past");

    /* Free blocks: a zero bit is free, and the cap stops the padding at the
     * end of a bitmap block being counted as space that does not exist. */
    memset(bitmap, 0, sizeof(bitmap));
    check(kfs_free_in(bitmap, sizeof(bitmap), 0u, 1000u) == 64u,
          "an empty bitmap counts every bit free");

    memset(bitmap, 0xFFu, sizeof(bitmap));
    check(kfs_free_in(bitmap, sizeof(bitmap), 0u, 1000u) == 0u,
          "a full bitmap counts none free");

    memset(bitmap, 0, sizeof(bitmap));
    bitmap[0] = 0x0Fu;                  /* four used, four free, in byte 0 */
    check(kfs_free_in(bitmap, 1u, 0u, 1000u) == 4u,
          "a part-used byte counts only its zero bits");

    memset(bitmap, 0, sizeof(bitmap));
    check(kfs_free_in(bitmap, sizeof(bitmap), 0u, 10u) == 10u,
          "the count stops at the blocks the volume really has");
}

static void test_fsinfo(void)
{
    unsigned char sector[512];
    struct fat_fsinfo info;

    memset(sector, 0, sizeof(sector));
    put32(sector, FAT_FSINFO_LEAD);
    put32(sector + 484, FAT_FSINFO_STRUCT);
    put32(sector + 508, FAT_FSINFO_TRAIL);
    put32(sector + 488, 12345u);
    put32(sector + 492, 2u);

    check(fat_fsinfo_from(sector, sizeof(sector), &info),
          "an FSInfo sector is recognised by all three signatures");
    check(info.free_clusters == 12345u && info.free_known,
          "its free count is read and known");
    check(info.next_free == 2u, "and where to look next");

    /* 0xFFFFFFFF is the specification's "unknown", and a reader must not
     * show four billion free clusters. */
    put32(sector + 488, FAT_FSINFO_UNKNOWN);
    check(fat_fsinfo_from(sector, sizeof(sector), &info) && !info.free_known,
          "a free count of 0xFFFFFFFF is unknown rather than enormous");

    /* One signature alone would match a reserved sector of zeroes. */
    memset(sector, 0, sizeof(sector));
    put32(sector, FAT_FSINFO_LEAD);
    check(!fat_fsinfo_from(sector, sizeof(sector), &info),
          "a lead signature alone is not an FSInfo sector");
}

static void test_names(void)
{
    char name[DRIVES_NAME_BYTES];
    const char *taken[4];

    drives_label_name(name, sizeof(name), "PHOTOS 2024");
    check(strcmp(name, "PHOTOS 2024") == 0, "a label is the name");

    drives_label_name(name, sizeof(name), "");
    check(strcmp(name, "Untitled") == 0, "no label at all is Untitled");

    drives_label_name(name, sizeof(name), NULL);
    check(strcmp(name, "Untitled") == 0, "and so is no label given");

    /* FAT pads a label with spaces, so a label of padding is no label. */
    drives_label_name(name, sizeof(name), "    ");
    check(strcmp(name, "Untitled") == 0,
          "a label of nothing but padding is Untitled");

    drives_label_name(name, sizeof(name), "HOLIDAY    ");
    check(strcmp(name, "HOLIDAY") == 0, "a label's trailing padding is cut");

    /* A `/` would make a second path component out of one volume. */
    drives_label_name(name, sizeof(name), "A/B");
    check(strcmp(name, "A_B") == 0, "a slash in a label cannot make a path");

    drives_label_name(name, sizeof(name), "CAF\xc9");
    check(strcmp(name, "CAF_") == 0,
          "a byte outside printable ASCII is shown as _, its code page "
          "being unrecorded");

    /* The numbering Diego chose: the first keeps the bare name. */
    taken[0] = "PHOTOS";
    strcpy(name, "PHOTOS");
    drives_unique_name(name, sizeof(name), taken, 1);
    check(strcmp(name, "PHOTOS 2") == 0, "a repeated label becomes PHOTOS 2");

    taken[1] = "PHOTOS 2";
    strcpy(name, "PHOTOS");
    drives_unique_name(name, sizeof(name), taken, 2);
    check(strcmp(name, "PHOTOS 3") == 0, "and the next PHOTOS 3");

    /* The same rule settles the unlabelled collision. */
    taken[0] = "Untitled";
    strcpy(name, "Untitled");
    drives_unique_name(name, sizeof(name), taken, 1);
    check(strcmp(name, "Untitled 2") == 0,
          "two blank sticks are Untitled and Untitled 2");

    /* Compared without regard to case, because FAT finds names that way and
     * two paths differing only in case cannot be told apart. */
    taken[0] = "photos";
    strcpy(name, "PHOTOS");
    drives_unique_name(name, sizeof(name), taken, 1);
    check(strcmp(name, "PHOTOS 2") == 0,
          "a name taken in another case is still taken");

    /* A name nobody holds is left exactly as it is. */
    taken[0] = "KOSMOS";
    strcpy(name, "PHOTOS");
    drives_unique_name(name, sizeof(name), taken, 1);
    check(strcmp(name, "PHOTOS") == 0, "an unused name is not numbered");
}

int main(void)
{
    test_mbr();
    test_gpt();
    test_kfs();
    test_fsinfo();
    test_names();

    if (failures > 0) {
        printf("FAIL: %d of %d checks on where a drive's volumes are\n",
               failures, checks + failures);
        return 1;
    }

    printf("PASS: %d checks on where a drive's volumes are and what they are "
           "called, on this machine (MBR and GPT tables, a protective MBR, "
           "kfs's superblock and its free blocks, FAT32's FSInfo hint, and "
           "Untitled with the numbering a repeated label gets).\n", checks);
    return 0;
}
