/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * FAT's boot sector, table and directory entries, asked the awkward questions
 * on the host (USB step 6, `docs/drives.html`).
 *
 * Every volume and entry here is built a byte at a time from Microsoft's FAT32
 * File System Specification 1.03: the four cluster counts either side of the
 * two FAT type boundaries, where "when it says <, it does not mean <="; a
 * boot sector with each field it is held to made wrong in turn; table entries
 * with the high four bits a FAT32 entry ignores; and long names whose pieces
 * arrive out of order, with another name's checksum, or with a surrogate that
 * lost its pair.
 *
 * What this cannot check is its own reading of the format - a wrong offset
 * built here and read there agrees with itself. `tools/test_fat.py` holds the
 * same code to volumes mtools made, which is somebody else's reading.
 */

#include <stdio.h>
#include <string.h>

#include "../user/servers/fat_decode.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

static void put16(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *at, uint32_t value)
{
    put16(at, value);
    put16(at + 2, value >> 16);
}

/*
 * A boot sector, laid out as the specification's tables lay one out. A FAT16
 * BPB when `fat16` is not 0, and a FAT32 one otherwise.
 */
static void boot(uint8_t *s, unsigned bps, unsigned spc, unsigned reserved,
                 unsigned fats, unsigned root_entries, uint32_t total,
                 unsigned fat16, uint32_t fat32, const char *label)
{
    memset(s, 0, 512);
    s[0] = 0xEB;
    s[1] = 0x3C;
    s[2] = 0x90;
    memcpy(s + 3, "MSWIN4.1", 8);
    put16(s + 11, bps);
    s[13] = (uint8_t)spc;
    put16(s + 14, reserved);
    s[16] = (uint8_t)fats;
    put16(s + 17, root_entries);

    if (fat16 != 0 && total < 0x10000u) {
        put16(s + 19, total);
    } else {
        put32(s + 32, total);
    }

    s[21] = 0xF8;

    if (fat16 != 0) {
        put16(s + 22, fat16);
        s[38] = 0x29;
        memcpy(s + 43, label, 11);
        memcpy(s + 54, "FAT16   ", 8);
    } else {
        put32(s + 36, fat32);
        put32(s + 44, 2);
        put16(s + 48, 1);
        put16(s + 50, 6);
        s[66] = 0x29;
        memcpy(s + 71, label, 11);
        memcpy(s + 82, "FAT32   ", 8);
    }

    s[510] = 0x55;
    s[511] = 0xAA;
}

/* The FAT16 volume most checks use: 128 MB, 8 sectors a cluster, 32731 clusters. */
static void fat16_boot(uint8_t *s)
{
    boot(s, 512, 8, 1, 2, 512, 262144, 128, 0, "KOSMOS DATA");
}

/* And the FAT32 one: 512 MB, 8 sectors a cluster, 130812 clusters. */
static void fat32_boot(uint8_t *s)
{
    boot(s, 512, 8, 32, 2, 0, 1048576, 0, 1024, "NO NAME    ");
}

static void volumes(void)
{
    uint8_t s[512];
    struct fat_volume v;
    const char *why = NULL;

    fat16_boot(s);
    put32(s + 39, 0x1A2B3C4Du);
    check(fat_volume_from(s, sizeof(s), &v, &why) && v.kind == FAT_16,
          "a 128 MB volume of 32731 clusters is read as FAT16");
    check(v.root_sectors == 32 && v.first_data_sector == 289
          && v.clusters == 32731 && v.cluster_bytes == 4096,
          "FAT16: RootDirSectors 32, FirstDataSector 289, 32731 clusters of 4 KB");
    check(fat_root_sector(&v) == 257, "FAT16's root directory begins at sector 257");
    check(fat_cluster_sector(&v, 2) == 289 && fat_cluster_sector(&v, 3) == 297,
          "FAT16: cluster 2 at sector 289, cluster 3 eight sectors on");
    check(strcmp(v.label, "KOSMOS DATA") == 0, "FAT16's label is KOSMOS DATA");
    check(v.has_serial && v.serial == 0x1A2B3C4Du,
          "FAT16's serial is BS_VolID, at byte 39, beside its label");

    fat32_boot(s);
    put32(s + 67, 0x0BADCAFEu);
    check(fat_volume_from(s, sizeof(s), &v, &why) && v.kind == FAT_32,
          "a 512 MB volume of 130812 clusters is read as FAT32");
    check(v.root_sectors == 0 && v.first_data_sector == 2080
          && v.clusters == 130812 && v.root_cluster == 2,
          "FAT32: no fixed root, FirstDataSector 2080, the root at cluster 2");
    check(v.label[0] == '\0', "a label of NO NAME is no label");
    check(v.has_serial && v.serial == 0x0BADCAFEu,
          "FAT32's serial is BS_VolID, at byte 67 - not FAT16's 39");

    /* Without BS_BootSig 0x29 the four bytes at 39 are not a serial, and a
     * volume must not be remembered by whatever happens to be there. */
    fat16_boot(s);
    put32(s + 39, 0x1A2B3C4Du);
    s[38] = 0;
    check(fat_volume_from(s, sizeof(s), &v, &why) && v.kind == FAT_16
          && !v.has_serial,
          "no BS_BootSig 0x29, no serial - whatever is at byte 39");

    /* The two boundaries, a cluster either side of each. */
    boot(s, 512, 1, 1, 1, 16, 4102, 16, 0, "LABEL      ");
    check(!fat_volume_from(s, sizeof(s), &v, &why) && v.kind == FAT_12,
          "4084 clusters is FAT12, recognised and refused");
    boot(s, 512, 1, 1, 1, 16, 4103, 16, 0, "LABEL      ");
    check(fat_volume_from(s, sizeof(s), &v, &why) && v.kind == FAT_16
          && v.clusters == 4085, "4085 clusters is FAT16");
    boot(s, 512, 1, 1, 1, 16, 65782, 256, 0, "LABEL      ");
    check(fat_volume_from(s, sizeof(s), &v, &why) && v.kind == FAT_16
          && v.clusters == 65524, "65524 clusters is FAT16, its sector count in BPB_TotSec32");
    boot(s, 512, 1, 32, 1, 0, 66069, 0, 512, "LABEL      ");
    check(fat_volume_from(s, sizeof(s), &v, &why) && v.kind == FAT_32
          && v.clusters == 65525, "65525 clusters is FAT32");

    /* Each field a boot sector is held to, made wrong in turn. */
    fat16_boot(s);
    s[510] = 0;
    check(!fat_volume_from(s, sizeof(s), &v, &why), "no 0x55 0xAA signature is refused");
    fat16_boot(s);
    s[0] = 0x00;
    check(!fat_volume_from(s, sizeof(s), &v, &why), "no jump at byte 0 is refused");
    fat16_boot(s);
    s[0] = 0xE9;
    s[2] = 0x00;
    check(fat_volume_from(s, sizeof(s), &v, &why), "the E9 ?? ?? jump is the other allowed form");
    fat16_boot(s);
    put16(s + 11, 1000);
    check(!fat_volume_from(s, sizeof(s), &v, &why), "1000 bytes a sector is refused");
    fat16_boot(s);
    s[13] = 3;
    check(!fat_volume_from(s, sizeof(s), &v, &why), "3 sectors a cluster is refused");
    fat16_boot(s);
    put16(s + 14, 0);
    check(!fat_volume_from(s, sizeof(s), &v, &why), "no reserved sectors is refused");
    fat16_boot(s);
    s[16] = 0;
    check(!fat_volume_from(s, sizeof(s), &v, &why), "no FATs is refused");
    fat16_boot(s);
    put32(s + 32, 0);
    check(!fat_volume_from(s, sizeof(s), &v, &why), "no sector count is refused");
    fat16_boot(s);
    put16(s + 17, 0);
    check(!fat_volume_from(s, sizeof(s), &v, &why), "a FAT16 volume with no root entries is refused");
    fat16_boot(s);
    put16(s + 22, 100);
    check(!fat_volume_from(s, sizeof(s), &v, &why), "a FAT too small for its clusters is refused");
    fat16_boot(s);
    put32(s + 32, 200);
    check(!fat_volume_from(s, sizeof(s), &v, &why), "a volume whose FATs fill it is refused");
    fat32_boot(s);
    put16(s + 17, 512);
    check(!fat_volume_from(s, sizeof(s), &v, &why), "FAT32 with BPB_RootEntCnt set is refused");
    fat32_boot(s);
    put16(s + 42, 1);
    check(!fat_volume_from(s, sizeof(s), &v, &why), "FAT32 version 0.1 is refused, as drivers must");
    fat32_boot(s);
    put32(s + 44, 130814);
    check(!fat_volume_from(s, sizeof(s), &v, &why), "a root cluster past the last is refused");
    check(why != NULL && strstr(why, "BPB_RootClus") != NULL, "and the refusal names BPB_RootClus");
    check(!fat_volume_from(s, 511, &v, &why), "a boot sector shorter than 512 bytes is refused");
}

static void tables(void)
{
    uint8_t s[512], at[4];
    struct fat_volume v16, v32;
    uint32_t sector = 0, offset = 0, next = 0;

    fat16_boot(s);
    fat_volume_from(s, sizeof(s), &v16, NULL);
    fat32_boot(s);
    fat_volume_from(s, sizeof(s), &v32, NULL);

    check(fat_entry_place(&v16, 2, &sector, &offset) && sector == 1 && offset == 4,
          "FAT16: cluster 2's entry is sector 1, byte 4");
    check(fat_entry_place(&v16, 300, &sector, &offset) && sector == 2 && offset == 88,
          "FAT16: cluster 300's entry is sector 2, byte 88");
    check(fat_entry_place(&v32, 2, &sector, &offset) && sector == 32 && offset == 8,
          "FAT32: cluster 2's entry is sector 32, byte 8");
    check(fat_entry_place(&v32, 200, &sector, &offset) && sector == 33 && offset == 288,
          "FAT32: cluster 200's entry is sector 33, byte 288");
    check(!fat_entry_place(&v16, 1, &sector, &offset)
          && !fat_entry_place(&v16, v16.clusters + 2, &sector, &offset),
          "clusters 1 and one past the last have no entry to place");

    put16(at, 0xFFF8);
    check(fat_link_at(&v16, at, &next) == FAT_LINK_END, "FAT16: 0xFFF8 ends a chain");
    put16(at, 0xFFFF);
    check(fat_link_at(&v16, at, &next) == FAT_LINK_END, "FAT16: 0xFFFF ends a chain");
    put16(at, 0xFFF7);
    check(fat_link_at(&v16, at, &next) == FAT_LINK_BAD, "FAT16: 0xFFF7 is a bad cluster");
    put16(at, 0);
    check(fat_link_at(&v16, at, &next) == FAT_LINK_FREE, "FAT16: 0 is free");
    put16(at, 1);
    check(fat_link_at(&v16, at, &next) == FAT_LINK_BROKEN, "FAT16: 1 is no cluster");
    put16(at, 3);
    check(fat_link_at(&v16, at, &next) == FAT_LINK_NEXT && next == 3, "FAT16: 3 leads to cluster 3");
    put16(at, v16.clusters + 2);
    check(fat_link_at(&v16, at, &next) == FAT_LINK_BROKEN, "FAT16: a cluster past the last is broken");

    put32(at, 0x0FFFFFFF);
    check(fat_link_at(&v32, at, &next) == FAT_LINK_END, "FAT32: 0x0FFFFFFF ends a chain");
    put32(at, 0xFFFFFFF8);
    check(fat_link_at(&v32, at, &next) == FAT_LINK_END, "FAT32: the high four bits are ignored at the end too");
    put32(at, 0x0FFFFFF7);
    check(fat_link_at(&v32, at, &next) == FAT_LINK_BAD, "FAT32: 0x0FFFFFF7 is a bad cluster");
    put32(at, 0x10000000);
    check(fat_link_at(&v32, at, &next) == FAT_LINK_FREE, "FAT32: 0x10000000 is free, as the specification's example says");
    put32(at, 0x30000005);
    check(fat_link_at(&v32, at, &next) == FAT_LINK_NEXT && next == 5, "FAT32: 0x30000005 leads to cluster 5");
}

/* A short entry: name, attributes, first cluster and size. */
static void short_entry(uint8_t *e, const char *name11, uint8_t attr,
                        uint16_t high, uint16_t low, uint32_t size)
{
    memset(e, 0, FAT_DIRENT_BYTES);
    memcpy(e, name11, 11);
    e[11] = attr;
    put16(e + 20, high);
    put16(e + 22, 0x6000);
    put16(e + 24, 0x5921);
    put16(e + 26, low);
    put32(e + 28, size);
}

/*
 * A long name laid out in UTF-16 as the pieces carry it: the name, a NUL, and
 * 0xFFFF to the end of its last piece - or neither, when it fills its pieces
 * exactly. Returns how many pieces it takes.
 */
static unsigned spelled(const uint16_t *name, unsigned length, uint16_t *units)
{
    unsigned pieces = (length + 12) / 13;

    for (unsigned i = 0; i < pieces * 13; i++) {
        units[i] = (i < length) ? name[i] : (i == length ? 0x0000 : 0xFFFF);
    }

    return pieces;
}

/* Long entry `ordinal`, carrying characters from `units` it covers. */
static void piece(uint8_t *e, uint8_t ordinal, int last, uint8_t checksum,
                  const uint16_t *units)
{
    unsigned first = (unsigned)((ordinal & 0x3F) - 1) * 13;

    memset(e, 0, FAT_DIRENT_BYTES);
    e[0] = (uint8_t)(ordinal | (last ? 0x40 : 0));
    e[11] = FAT_ATTR_LONG_NAME;
    e[13] = checksum;

    for (unsigned i = 0; i < 5; i++) put16(e + 1 + 2 * i, units[first + i]);
    for (unsigned i = 0; i < 6; i++) put16(e + 14 + 2 * i, units[first + 5 + i]);
    for (unsigned i = 0; i < 2; i++) put16(e + 28 + 2 * i, units[first + 11 + i]);
}

static void ascii16(const char *text, uint16_t *out, unsigned *length)
{
    unsigned n = 0;

    while (text[n] != '\0') {
        out[n] = (uint8_t)text[n];
        n++;
    }

    *length = n;
}

/* Runs a long name's pieces, last first as they are stored, then its short entry. */
static enum fat_step named(const struct fat_volume *v, const uint16_t *name,
                           unsigned length, const char *short11, int wrong_sum,
                           struct fat_dirent *d)
{
    uint16_t units[260];
    uint8_t e[FAT_DIRENT_BYTES];
    struct fat_names names;
    unsigned pieces = spelled(name, length, units);
    uint8_t sum = fat_short_checksum((const uint8_t *)short11);
    enum fat_step step = FAT_STEP_SKIP;

    fat_names_clear(&names);

    for (unsigned k = pieces; k >= 1; k--) {
        piece(e, (uint8_t)k, k == pieces, (uint8_t)(wrong_sum ? sum + 1 : sum), units);
        step = fat_dirent_step(v, e, &names, d);

        if (step != FAT_STEP_PIECE) {
            return step;
        }
    }

    short_entry(e, short11, FAT_ATTR_ARCHIVE, 0, 9, 42);
    return fat_dirent_step(v, e, &names, d);
}

static void entries(void)
{
    uint8_t s[512], e[FAT_DIRENT_BYTES];
    struct fat_volume v16, v32;
    struct fat_names names;
    struct fat_dirent d;
    uint16_t name[64];
    unsigned length;

    fat16_boot(s);
    fat_volume_from(s, sizeof(s), &v16, NULL);
    fat32_boot(s);
    fat_volume_from(s, sizeof(s), &v32, NULL);
    fat_names_clear(&names);

    short_entry(e, "README  TXT", FAT_ATTR_ARCHIVE, 0, 5, 1234);
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_ENTRY
          && strcmp(d.name, "README.TXT") == 0 && !d.long_name
          && d.first_cluster == 5 && d.size == 1234 && !d.directory,
          "README  TXT is the file README.TXT, cluster 5, 1234 bytes");
    check(d.write_time == 0x6000 && d.write_date == 0x5921, "and its write time and date are kept");

    short_entry(e, "PHOTOS     ", FAT_ATTR_DIRECTORY, 0, 7, 0);
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_ENTRY
          && strcmp(d.name, "PHOTOS") == 0 && d.directory,
          "PHOTOS with ATTR_DIRECTORY is a directory with no dot");

    short_entry(e, "MAKEFILE   ", FAT_ATTR_ARCHIVE, 0, 8, 1);
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_ENTRY
          && strcmp(d.name, "MAKEFILE") == 0, "a name with no extension has no dot");

    short_entry(e, "HELLO   TXT", FAT_ATTR_ARCHIVE, 0, 5, 6);
    e[12] = 0x18;
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_ENTRY
          && strcmp(d.name, "hello.txt") == 0 && strcmp(d.short_name, "HELLO.TXT") == 0,
          "DIR_NTRes 0x18 shows hello.txt, and HELLO.TXT stays the stored name");
    e[12] = 0x08;
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_ENTRY
          && strcmp(d.name, "hello.TXT") == 0, "DIR_NTRes 0x08 lowers the base name only");
    e[12] = 0x10;
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_ENTRY
          && strcmp(d.name, "HELLO.txt") == 0, "DIR_NTRes 0x10 lowers the extension only");

    short_entry(e, "\x05" "BC     TXT", FAT_ATTR_ARCHIVE, 0, 8, 1);
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_ENTRY
          && strcmp(d.name, "_BC.TXT") == 0,
          "0x05 in DIR_Name[0] is not free: it stands for 0xE5, a code page character shown as _");

    short_entry(e, "CAF\x82    TXT", FAT_ATTR_ARCHIVE, 0, 8, 1);
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_ENTRY
          && strcmp(d.name, "CAF_.TXT") == 0,
          "a byte above 0x7F in a short name is shown as _");

    short_entry(e, "\xE5" "ONE    TXT", FAT_ATTR_ARCHIVE, 0, 8, 1);
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_SKIP, "0xE5 in DIR_Name[0] is a free entry");

    memset(e, 0, sizeof(e));
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_END, "0x00 in DIR_Name[0] ends the directory");

    short_entry(e, ".          ", FAT_ATTR_DIRECTORY, 0, 7, 0);
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_SKIP, "the dot entry is not shown");
    short_entry(e, "..         ", FAT_ATTR_DIRECTORY, 0, 0, 0);
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_SKIP, "the dotdot entry is not shown");

    short_entry(e, "KOSMOS     ", FAT_ATTR_VOLUME_ID, 0, 0, 0);
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_LABEL
          && strcmp(d.name, "KOSMOS") == 0, "ATTR_VOLUME_ID alone is the label KOSMOS");

    short_entry(e, "ODD        ", FAT_ATTR_VOLUME_ID | FAT_ATTR_DIRECTORY, 0, 0, 0);
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_SKIP,
          "a directory that is also a label is an invalid entry, and skipped");

    short_entry(e, "BIG     BIN", FAT_ATTR_ARCHIVE, 0x0001, 0x0002, 9);
    check(fat_dirent_step(&v32, e, &names, &d) == FAT_STEP_ENTRY
          && d.first_cluster == 0x10002, "FAT32: DIR_FstClusHI gives cluster 0x10002");
    check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_ENTRY
          && d.first_cluster == 2, "FAT16: DIR_FstClusHI is ignored");

    /* Long names. */
    ascii16("The quick brown.fox", name, &length);
    check(named(&v32, name, length, "THEQUI~1FOX", 0, &d) == FAT_STEP_ENTRY
          && d.long_name && strcmp(d.name, "The quick brown.fox") == 0
          && strcmp(d.short_name, "THEQUI~1.FOX") == 0,
          "two pieces spell The quick brown.fox, beside its short name");

    check(named(&v32, name, length, "THEQUI~1FOX", 1, &d) == FAT_STEP_ENTRY
          && !d.long_name && strcmp(d.name, "THEQUI~1.FOX") == 0,
          "pieces with another name's checksum are orphans: the short name is shown");

    ascii16("Exactly13char", name, &length);
    check(named(&v16, name, length, "EXACTL~1   ", 0, &d) == FAT_STEP_ENTRY
          && d.long_name && strcmp(d.name, "Exactly13char") == 0,
          "a name that fills its piece exactly needs no NUL");

    name[0] = 'C'; name[1] = 'a'; name[2] = 'f'; name[3] = 0x00E9;
    name[4] = ' '; name[5] = 0xD83D; name[6] = 0xDE00;
    check(named(&v16, name, 7, "CAF~1      ", 0, &d) == FAT_STEP_ENTRY
          && strcmp(d.name, "Caf\xC3\xA9 \xF0\x9F\x98\x80") == 0,
          "U+00E9 is two bytes of UTF-8, and a surrogate pair is one four-byte character");

    name[0] = 'a'; name[1] = 0xD83D; name[2] = 'b';
    check(named(&v16, name, 3, "A~1        ", 0, &d) == FAT_STEP_ENTRY
          && strcmp(d.name, "a_b") == 0, "a surrogate without its pair is shown as _");

    {
        uint16_t units[260];
        uint8_t sum;

        ascii16("The quick brown.fox", name, &length);
        spelled(name, length, units);
        sum = fat_short_checksum((const uint8_t *)"THEQUI~1FOX");
        fat_names_clear(&names);

        piece(e, 1, 0, sum, units);
        check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_SKIP,
              "a first piece before the last one is an orphan");

        piece(e, 2, 1, sum, units);
        fat_dirent_step(&v16, e, &names, &d);
        e[0] = 0xE5;
        check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_SKIP, "a free entry inside a set");
        short_entry(e, "THEQUI~1FOX", FAT_ATTR_ARCHIVE, 0, 9, 42);
        check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_ENTRY && !d.long_name,
              "ends the set: its short entry keeps the short name");

        piece(e, 1, 1, sum, units);
        e[12] = 1;
        check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_SKIP,
              "a long entry whose LDIR_Type is not zero is not a name's piece");

        piece(e, 21, 1, sum, units);
        check(fat_dirent_step(&v16, e, &names, &d) == FAT_STEP_SKIP,
              "a set of 21 pieces, more than 255 characters take, is not begun");
    }

    check(fat_name_matches("readme.txt", "README.TXT"), "readme.txt finds README.TXT");
    check(!fat_name_matches("readme.tx", "README.TXT"), "readme.tx does not");
    check(!fat_name_matches("caf\xc3\xa9", "CAF\xc3\x89"), "accented letters compare exactly, as the header says");
}

int main(void)
{
    volumes();
    tables();
    entries();

    if (fails) {
        printf("FAIL: %d of %d checks on FAT's boot sector, table and directory entries.\n",
               fails, fails + checks);
        return 1;
    }

    printf("PASS: %d checks on FAT's boot sector, table and directory entries "
           "(both type boundaries a cluster either side, each field a boot sector "
           "is held to, FAT32's ignored high bits, and long names out of order, "
           "orphaned or in UTF-16 surrogates).\n", checks);
    return 0;
}
