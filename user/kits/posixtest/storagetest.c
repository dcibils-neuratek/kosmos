/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Storage Kit's checks** (`docs/w3.md` W3d1), in `posixtest.elf`
 * beside the POSIX layer's: what `open`, `read` and `write` will stand on,
 * held against the three servers it speaks to - `/Home` (diskproto),
 * `/Temporary` (ramproto) and `/Kosmos` (binproto) - from four threads at
 * once as well as one.
 *
 * `kosmos_storagetest_run` writes one sentence: every check's result when
 * all of them held, or the first that did not and why.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage.h"

#define SAY(...) do { snprintf(out, size, __VA_ARGS__); return 1; } while (0)

/* Where the loader puts this program and its image. */
#define OWN_FOLDER "/Home/apps/apptest"
#define OWN_SOURCE OWN_FOLDER "/storagetest.lua"

#define BIG 3000                        /* three of `/Home`'s messages */

static unsigned char pattern(size_t i)
{
    return (unsigned char)(i * 7u + 3u);
}

/* Every name in `dir`, a few at a time, and whether `want` is among them. */
static long names_in(const char *dir, const char *want, int *found)
{
    char names[48];
    uint64_t cursor = 0;
    long total = 0, n;

    *found = 0;

    while ((n = storage_list(dir, &cursor, names, sizeof names)) > 0) {
        const char *p = names;
        long i;

        for (i = 0; i < n; i++) {
            if (strcmp(p, want) == 0) {
                *found = 1;
            }

            p += strlen(p) + 1;
        }

        total += n;
    }

    return n < 0 ? n : total;
}

/* The same checks on one writable mount's folder. */
static int files_in(const char *dir, char *out, unsigned long size)
{
    char path[STORAGE_PATH_MAX], sub[STORAGE_PATH_MAX], moved[STORAGE_PATH_MAX];
    unsigned char *big = malloc(BIG), *back = malloc(BIG);
    struct storage_stat st;
    unsigned char got[32];
    long r, n = 0;
    int found;
    size_t i;

    if (big == NULL || back == NULL) {
        SAY("storage: no memory for %s's checks", dir);
    }

    snprintf(path, sizeof path, "%s/st-file", dir);
    snprintf(sub, sizeof sub, "%s/st-dir", dir);
    snprintf(moved, sizeof moved, "%s/st-dir/moved", dir);
    (void)storage_delete(moved);
    (void)storage_delete(sub);
    (void)storage_delete(path);

    /* Not made without being asked to. */
    if ((r = storage_write(path, 0, "x", 1, 0)) != STORAGE_ERR_NO_FILE) {
        SAY("storage: %s: a write without CREATE to a file not there said %ld", dir, r);
    }

    if ((r = storage_write(path, 0, "hello world", 11, STORAGE_CREATE)) != 11) {
        SAY("storage: %s: writing 11 bytes said %ld", dir, r);
    }

    if ((r = storage_read(path, 0, got, sizeof got)) != 11 || memcmp(got, "hello world", 11) != 0) {
        SAY("storage: %s: reading them back said %ld", dir, r);
    }

    /* A byte laid over the middle, and the rest kept. */
    if ((r = storage_write(path, 6, "J", 1, 0)) != 1
        || (n = storage_read(path, 0, got, sizeof got)) != 11 || memcmp(got, "hello Jorld", 11) != 0) {
        SAY("storage: %s: after a byte at 6 the file read as %ld bytes (the write said %ld)",
            dir, n, r);
    }

    /* And at 0, which is where a server that truncates on a write would. */
    if ((r = storage_write(path, 0, "H", 1, 0)) != 1
        || (n = storage_read(path, 0, got, sizeof got)) != 11 || memcmp(got, "Hello Jorld", 11) != 0) {
        SAY("storage: %s: after a byte at 0 the file read as %ld bytes (the write said %ld)",
            dir, n, r);
    }

    if (storage_stat(path, &st) != STORAGE_OK || st.kind != STORAGE_KIND_FILE || st.size != 11) {
        SAY("storage: %s: the file is kind %u, %llu bytes, not a file of 11", dir,
            st.kind, (unsigned long long)st.size);
    }

    /* Several messages each way, and the end where the bytes end. */
    for (i = 0; i < BIG; i++) {
        big[i] = pattern(i);
    }

    if ((r = storage_write(path, 0, big, BIG, STORAGE_CUT)) != BIG) {
        SAY("storage: %s: writing %d bytes said %ld", dir, BIG, r);
    }

    memset(back, 0, BIG);

    if ((r = storage_read(path, 0, back, BIG)) != BIG || memcmp(big, back, BIG) != 0) {
        SAY("storage: %s: %d bytes read back as %ld, or not as written", dir, BIG, r);
    }

    if (storage_read(path, BIG, got, sizeof got) != 0) {
        SAY("storage: %s: a read at the end was not 0", dir);
    }

    /* Shorter, and longer again with nothing in the gap but zeros. */
    if (storage_truncate(path, 5) != STORAGE_OK || storage_stat(path, &st) != STORAGE_OK
        || st.size != 5) {
        SAY("storage: %s: cut to 5, the file is %llu bytes", dir, (unsigned long long)st.size);
    }

    if (storage_truncate(path, 10) != STORAGE_OK || storage_stat(path, &st) != STORAGE_OK
        || st.size != 10 || storage_read(path, 0, got, sizeof got) != 10
        || memcmp(got, big, 5) != 0 || memcmp(got + 5, "\0\0\0\0\0", 5) != 0) {
        SAY("storage: %s: grown to 10, the file is %llu bytes, or not zeros past 5", dir,
            (unsigned long long)st.size);
    }

    /* Folders. */
    if ((r = storage_mkdir(sub)) != STORAGE_OK) {
        SAY("storage: %s: mkdir said %ld", dir, r);
    }

    if ((r = storage_mkdir(sub)) != STORAGE_ERR_EXISTS) {
        SAY("storage: %s: a second mkdir said %ld", dir, r);
    }

    if (storage_stat(sub, &st) != STORAGE_OK || st.kind != STORAGE_KIND_DIR) {
        SAY("storage: %s: the folder is not a folder", dir);
    }

    if ((r = storage_write(path, 0, NULL, 0, 0)) != 0) {
        SAY("storage: %s: an empty write said %ld", dir, r);
    }

    {
        char inside[STORAGE_PATH_MAX];

        snprintf(inside, sizeof inside, "%s/st-none/x", dir);

        if ((r = storage_write(inside, 0, "x", 1, STORAGE_CREATE)) != STORAGE_ERR_NO_FILE) {
            SAY("storage: %s: a file in a folder not there said %ld", dir, r);
        }
    }

    if ((r = storage_rename(path, moved)) != STORAGE_OK) {
        SAY("storage: %s: rename into the folder said %ld", dir, r);
    }

    if (storage_stat(path, &st) != STORAGE_ERR_NO_FILE) {
        SAY("storage: %s: the renamed file is still where it was", dir);
    }

    if ((r = names_in(sub, "moved", &found)) != 1 || !found) {
        SAY("storage: %s: the folder lists %ld names, %s", dir, r,
            found ? "\"moved\" among them" : "not \"moved\"");
    }

    if ((r = storage_delete(sub)) != STORAGE_ERR_NOT_EMPTY) {
        SAY("storage: %s: deleting a folder with a file in it said %ld", dir, r);
    }

    if (storage_delete(moved) != STORAGE_OK || storage_delete(sub) != STORAGE_OK
        || storage_stat(sub, &st) != STORAGE_ERR_NO_FILE) {
        SAY("storage: %s: the file and the folder were not deleted", dir);
    }

    if (storage_flush(dir) != STORAGE_OK) {
        SAY("storage: %s: flush refused", dir);
    }

    free(big);
    free(back);
    return 0;
}

/* Four threads, each with a file of its own, written and read at once. */
#define THREADS 4
#define ROUNDS  25

static void *churns(void *arg)
{
    intptr_t me = (intptr_t)arg;
    char path[64];
    unsigned char mine[700], back[700];
    int round;
    size_t i;

    snprintf(path, sizeof path, "%s/st-thread-%d", (me & 1) ? "/Temporary" : OWN_FOLDER,
             (int)me);

    for (round = 0; round < ROUNDS; round++) {
        for (i = 0; i < sizeof mine; i++) {
            mine[i] = (unsigned char)(me * 31 + round + i);
        }

        if (storage_write(path, 0, mine, sizeof mine, STORAGE_CREATE | STORAGE_CUT)
                != (long)sizeof mine
            || storage_read(path, 0, back, sizeof back) != (long)sizeof back
            || memcmp(mine, back, sizeof mine) != 0) {
            return (void *)(intptr_t)(round + 1);
        }
    }

    (void)storage_delete(path);
    return NULL;
}

int kosmos_storagetest_run(char *out, unsigned long size)
{
    struct storage_mount none[1];
    struct storage_stat st;
    char whole[STORAGE_PATH_MAX], first[64];
    unsigned char *source;
    long r, total, listed, own;
    int found;

    /* 1: the table is the runner's, and a program cannot hand itself one */
    memset(none, 0, sizeof none);

    if ((r = storage_start(none, 0, "/")) != STORAGE_ERR_DENIED) {
        SAY("storage: a second table was not refused (%ld)", r);
    }

    /* 2: paths made whole */
    if (storage_absolute("/a//b/./c/../d/", whole) != STORAGE_OK || strcmp(whole, "/a/b/d") != 0) {
        SAY("storage: \"/a//b/./c/../d/\" was made \"%s\"", whole);
    }

    if (storage_absolute("/../..", whole) != STORAGE_OK || strcmp(whole, "/") != 0) {
        SAY("storage: \"/../..\" was made \"%s\"", whole);
    }

    {
        char want[STORAGE_PATH_MAX];

        snprintf(want, sizeof want, "%s/x", strcmp(storage_cwd(), "/") == 0 ? "" : storage_cwd());

        if (storage_absolute("x", whole) != STORAGE_OK || strcmp(whole, want) != 0) {
            SAY("storage: \"x\" in %s was made \"%s\"", storage_cwd(), whole);
        }
    }

    /* 3: what was not handed over is not there */
    if ((r = storage_stat("/Nowhere/x", &st)) != STORAGE_ERR_NO_MOUNT) {
        SAY("storage: a path under no mount said %ld", r);
    }

    /* 4: this program's own source, by its name and by another case */
    if ((r = storage_stat(OWN_SOURCE, &st)) != STORAGE_OK || st.kind != STORAGE_KIND_FILE) {
        SAY("storage: %s is not a file (%ld)", OWN_SOURCE, r);
    }

    if ((source = malloc((size_t)st.size + 1)) == NULL) {
        SAY("storage: no memory for the source");
    }

    total = storage_read("/HOME/apps/apptest/storagetest.lua", 0, source, (size_t)st.size + 1);

    if (total != (long)st.size || memcmp(source, "-- Kosmos.", 10) != 0) {
        SAY("storage: the source read as %ld bytes of %llu", total, (unsigned long long)st.size);
    }

    own = total;
    free(source);

    /* 5, 6: a file's life, on the disk and in memory */
    if (files_in(OWN_FOLDER, out, size) != 0 || files_in("/Temporary", out, size) != 0) {
        return 1;
    }

    /* 7: /Kosmos, read and never written */
    if (storage_stat("/Kosmos/Programs", &st) != STORAGE_OK || st.kind != STORAGE_KIND_DIR) {
        SAY("storage: /Kosmos/Programs is not a folder");
    }

    {
        char names[64];
        uint64_t cursor = 0;

        if (storage_list("/Kosmos/Programs", &cursor, names, sizeof names) < 1) {
            SAY("storage: /Kosmos/Programs lists nothing");
        }

        snprintf(first, sizeof first, "/Kosmos/Programs/%s", names);
        listed = names_in("/Kosmos/Programs", names, &found);

        if (listed < 10 || !found) {
            SAY("storage: /Kosmos/Programs lists %ld names", listed);
        }
    }

    if (storage_stat(first, &st) != STORAGE_OK || st.kind != STORAGE_KIND_FILE || st.size == 0) {
        SAY("storage: %s is not a file with bytes in it", first);
    }

    if ((source = malloc((size_t)st.size)) == NULL) {
        SAY("storage: no memory for %s", first);
    }

    total = storage_read(first, 0, source, (size_t)st.size);
    free(source);

    if (total != (long)st.size) {
        SAY("storage: %s read as %ld bytes of %llu", first, total, (unsigned long long)st.size);
    }

    if ((r = storage_write(first, 0, "x", 1, 0)) != STORAGE_ERR_READ_ONLY
        || storage_mkdir("/Kosmos/Programs/x") != STORAGE_ERR_READ_ONLY) {
        SAY("storage: a write to /Kosmos said %ld", r);
    }

    /* 8: a rename does not cross from one server to another */
    if ((r = storage_rename("/Temporary/a", OWN_FOLDER "/a")) != STORAGE_ERR_CROSSES) {
        SAY("storage: a rename across mounts said %ld", r);
    }

    /* 9: from four threads at once */
    {
        pthread_t t[THREADS];
        void *got;
        intptr_t i;

        for (i = 0; i < THREADS; i++) {
            pthread_create(&t[i], NULL, churns, (void *)i);
        }

        for (i = 0; i < THREADS; i++) {
            pthread_join(t[i], &got);

            if (got != NULL) {
                SAY("storage: thread %ld's file was not as written in round %ld", (long)i,
                    (long)(intptr_t)got);
            }
        }
    }

    snprintf(out, size, "storage: refused a second table, paths, no mount, own source "
             "%ld bytes, /Home and /Temporary written, cut, grown, folders, renamed, deleted; "
             "/Kosmos %ld programs, read only; crosses refused; %d threads",
             own, listed, THREADS);
    return 0;
}
