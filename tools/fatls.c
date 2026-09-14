/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A FAT volume inside an image file, walked on this Mac with the same
 * `fat_decode.c` the drive server will use, so `tools/test_fat.py` can hold
 * it to the files mtools put there.
 *
 *   fatls IMAGE SECTOR                every directory and file: sizes, a
 *                                     64-bit FNV-1a of each file's bytes,
 *                                     and how many runs its chain is in
 *   fatls IMAGE SECTOR --find PATH    one file, found as FAT finds names -
 *                                     long or short, without regard to case
 *
 * SECTOR is where the volume begins, counted in 512-byte sectors: 0 for a
 * drive with no partition table.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/servers/fat_decode.h"

static FILE *image;
static long long base;                  /* the volume's first byte */
static struct fat_volume volume;
static uint8_t *table;                  /* the first FAT, whole */

static void die(const char *what, const char *why)
{
    fprintf(stderr, "fatls: %s: %s\n", what, why);
    exit(1);
}

static int read_sectors(uint32_t sector, uint32_t count, uint8_t *into)
{
    long long at = base + (long long)sector * volume.bytes_per_sector;

    return fseeko(image, (off_t)at, SEEK_SET) == 0
        && fread(into, volume.bytes_per_sector, count, image) == count;
}

/* 1 and `next` for a following cluster, 0 at the chain's end, -1 with `why`. */
static int follow(uint32_t cluster, uint32_t *next, const char **why)
{
    uint32_t sector, offset;

    if (!fat_entry_place(&volume, cluster, &sector, &offset)) {
        *why = "a chain names a cluster the volume does not have";
        return -1;
    }

    switch (fat_link_at(&volume,
                        table + (sector - volume.reserved_sectors)
                              * (size_t)volume.bytes_per_sector + offset,
                        next)) {
    case FAT_LINK_NEXT:  return 1;
    case FAT_LINK_END:   return 0;
    case FAT_LINK_FREE:  *why = "a chain runs into a free cluster"; return -1;
    case FAT_LINK_BAD:   *why = "a chain runs into a bad cluster"; return -1;
    default:             *why = "a chain points past the volume"; return -1;
    }
}

/*
 * A directory's entries, one call each. `first` is its first cluster, or 0
 * for FAT16's fixed root. Stops when `visit` returns non-zero.
 */
typedef int (*visitor)(const struct fat_dirent *d, void *context);

static void each_entry(uint32_t first, visitor visit, void *context)
{
    struct fat_names names;
    struct fat_dirent d;
    const char *why = NULL;
    uint32_t cluster = first, steps = 0;
    uint32_t sectors = (first == 0) ? volume.root_sectors : volume.sectors_per_cluster;
    uint8_t *buffer = malloc((size_t)sectors * volume.bytes_per_sector);

    fat_names_clear(&names);

    for (;;) {
        uint32_t at = (first == 0) ? fat_root_sector(&volume)
                                   : fat_cluster_sector(&volume, cluster);

        if (!read_sectors(at, sectors, buffer)) {
            die("a directory", "could not be read from the image");
        }

        for (uint32_t i = 0; i < sectors * volume.bytes_per_sector; i += FAT_DIRENT_BYTES) {
            enum fat_step step = fat_dirent_step(&volume, buffer + i, &names, &d);

            if (step == FAT_STEP_END) {
                free(buffer);
                return;
            }

            if (step == FAT_STEP_ENTRY && visit(&d, context) != 0) {
                free(buffer);
                return;
            }
        }

        if (first == 0) {
            break;
        }

        int more = follow(cluster, &cluster, &why);

        if (more < 0) {
            die("a directory's chain", why);
        }

        if (more == 0 || ++steps > volume.clusters) {
            break;
        }
    }

    free(buffer);
}

/* A file's bytes hashed; its chain's runs counted. */
static void file_summary(const struct fat_dirent *d, unsigned long long *hash,
                         unsigned *runs)
{
    uint8_t *buffer = malloc(volume.cluster_bytes);
    uint32_t cluster = d->first_cluster, left = d->size, steps = 0, previous = 0;
    const char *why = NULL;

    *hash = 0xcbf29ce484222325ULL;
    *runs = 0;

    while (left > 0) {
        uint32_t take = left < volume.cluster_bytes ? left : volume.cluster_bytes;

        if (cluster < 2 || cluster > volume.clusters + 1) {
            die(d->name, "its chain leaves the volume before its size is read");
        }

        if (*runs == 0 || cluster != previous + 1) {
            (*runs)++;
        }

        if (!read_sectors(fat_cluster_sector(&volume, cluster),
                          volume.sectors_per_cluster, buffer)) {
            die(d->name, "a cluster could not be read from the image");
        }

        for (uint32_t i = 0; i < take; i++) {
            *hash = (*hash ^ buffer[i]) * 0x100000001b3ULL;
        }

        left -= take;
        previous = cluster;

        if (left > 0) {
            int more = follow(cluster, &cluster, &why);

            if (more <= 0 || ++steps > volume.clusters) {
                die(d->name, more < 0 ? why : "its chain ends before its size");
            }
        }
    }

    free(buffer);
}

struct walk {
    char path[4096];
};

static int list_one(const struct fat_dirent *d, void *context)
{
    struct walk *w = context;
    size_t length = strlen(w->path);

    snprintf(w->path + length, sizeof(w->path) - length, "/%s", d->name);

    if (d->directory) {
        printf("dir %s\n", w->path);
        each_entry(d->first_cluster, list_one, w);
    } else {
        unsigned long long hash;
        unsigned runs;

        file_summary(d, &hash, &runs);
        printf("file %s %u %016llx %u\n", w->path, d->size, hash, runs);
    }

    w->path[length] = '\0';
    return 0;
}

struct search {
    const char *want;
    struct fat_dirent found;
    int hit;
};

static int find_one(const struct fat_dirent *d, void *context)
{
    struct search *s = context;

    /* "A long name search operation checks both the long and short directory entries." */
    if (fat_name_matches(s->want, d->name) || fat_name_matches(s->want, d->short_name)) {
        s->found = *d;
        s->hit = 1;
        return 1;
    }

    return 0;
}

static void find(char *path)
{
    uint32_t directory = volume.kind == FAT_32 ? volume.root_cluster : 0;
    char shown[4096] = "";
    char *part = strtok(path, "/");

    while (part != NULL) {
        struct search s = { part, { 0 }, 0 };
        char *rest = strtok(NULL, "/");

        each_entry(directory, find_one, &s);

        if (!s.hit) {
            printf("not found %s\n", part);
            return;
        }

        strncat(shown, "/", sizeof(shown) - strlen(shown) - 1);
        strncat(shown, s.found.name, sizeof(shown) - strlen(shown) - 1);

        if (rest == NULL) {
            unsigned long long hash;
            unsigned runs;

            file_summary(&s.found, &hash, &runs);
            printf("found %s %u %016llx\n", shown, s.found.size, hash);
            return;
        }

        if (!s.found.directory) {
            printf("not a directory %s\n", shown);
            return;
        }

        directory = s.found.first_cluster;
        part = rest;
    }
}

int main(int argc, char **argv)
{
    uint8_t boot[4096];
    const char *why = NULL;
    struct walk w = { "" };

    if (argc < 3) {
        fprintf(stderr, "usage: fatls IMAGE SECTOR [--find PATH]\n");
        return 2;
    }

    image = fopen(argv[1], "rb");

    if (image == NULL) {
        die(argv[1], "could not be opened");
    }

    base = strtoll(argv[2], NULL, 10) * 512LL;

    if (fseeko(image, (off_t)base, SEEK_SET) != 0 || fread(boot, 512, 1, image) != 1) {
        die(argv[1], "no boot sector at that sector");
    }

    if (!fat_volume_from(boot, 512, &volume, &why)) {
        die("the boot sector", why);
    }

    table = malloc((size_t)volume.fat_sectors * volume.bytes_per_sector);

    if (table == NULL || !read_sectors(volume.reserved_sectors, volume.fat_sectors, table)) {
        die("the FAT", "could not be read from the image");
    }

    printf("kind %s clusters %u cluster_bytes %u label %s\n",
           fat_kind_name(volume.kind), volume.clusters, volume.cluster_bytes,
           volume.label);

    if (argc >= 5 && strcmp(argv[3], "--find") == 0) {
        find(argv[4]);
    } else {
        each_entry(volume.kind == FAT_32 ? volume.root_cluster : 0, list_one, &w);
    }

    return 0;
}
