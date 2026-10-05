/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /Temporary's store, held on the host (`user/servers/ramstore.c`).
 *
 * The server is a loop round this, and the store is what decides what a
 * request does - so this drives it with the requests the namespace sends,
 * as `struct ram_request`s, and reads the `struct ram_reply`s it answers
 * with. What it holds the store to is step 4 after 0.11 (5 October 2026):
 * the 128 entries and the 16 KB a value that were compiled in are gone, and
 * a ceiling is what refuses.
 *
 *   - five hundred files, listed in order a page at a time, and read back;
 *   - a value of 300 KB, written a message at a time and read back whole;
 *   - a gap written past the end reads as zeros, not as old bytes;
 *   - the ceiling refuses, leaves no empty file behind, and keeps what it
 *     held readable; a delete gives the room back and a write fits again;
 *   - a large value written again from the start gives its room back, and
 *     a small one keeps it;
 *   - forty watches parked at once, all answered by one write;
 *   - a directory renamed with what is in it;
 *   - an offset near the top of 32 bits refused rather than wrapped;
 *   - and after everything is deleted, nothing is held.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ramstore.h"

static unsigned checks, failed;

static void check(bool ok, const char *what)
{
    checks++;

    if (!ok) {
        failed++;
        printf("FAIL: %s\n", what);
    }
}

/* What the store last said, and to whom it has said anything. */
static struct ram_reply said;
static uint64_t told[256];
static unsigned ntold;

void ram_reply(uint64_t to, const struct ram_reply *rep)
{
    said = *rep;

    if (ntold < sizeof(told) / sizeof(told[0])) {
        told[ntold] = to;
    }

    ntold++;
}

/* A request with only a path, an offset and data: the shape of most. */
static uint32_t ask(uint32_t op, const char *path, uint32_t offset,
                    const void *data, uint32_t length, uint64_t sender)
{
    static struct ram_request req;
    unsigned before = ntold;

    memset(&req, 0, sizeof(req));
    req.op = op;
    req.offset = offset;
    req.length = length;
    snprintf(req.path, sizeof(req.path), "%s", path);

    if (data != NULL && length > 0) {
        memcpy(req.u.data, data, length);
    }

    memset(&said, 0, sizeof(said));
    ram_answer(&req, sizeof(req), sender);

    /* No answer at all is a parked watch, which says so by error 0 and an
     * untouched count. */
    return (ntold == before) ? 0xffffffffu : said.error;
}

/* Text written from the start, in one message. */
static uint32_t put(const char *path, const char *text)
{
    return ask(RAM_OP_WRITE, path, 0, text, (uint32_t)strlen(text), 1);
}

/* The whole of a value, a message at a time, into `out`. */
static size_t get(const char *path, char *out, size_t cap)
{
    size_t got = 0;

    for (;;) {
        if (ask(RAM_OP_READ, path, (uint32_t)got, NULL, 0, 1) != RAM_OK) {
            return (size_t)-1;
        }

        if (got + said.length > cap) {
            return (size_t)-1;
        }

        memcpy(out + got, said.u.data, said.length);
        got += said.length;

        if (!said.more) {
            return got;
        }
    }
}

static void many_files(void)
{
    char path[64], text[64], back[64], last[RAM_PATH_MAX] = "";
    unsigned i, listed = 0, offset = 0;
    bool ordered = true;
    size_t n;

    ram_store_init(64u * 1024u * 1024u);

    for (i = 0; i < 500; i++) {
        snprintf(path, sizeof(path), "/many/f%03u", i);
        snprintf(text, sizeof(text), "file %u", i);

        if (put(path, text) != RAM_OK) {
            break;
        }
    }

    check(i == 500, "five hundred files written, where 128 was the most");
    check(ram_store_entries() == 501, "five hundred files and their folder");

    /* Listed four at a time, as the namespace asks, and in order. */
    for (;;) {
        unsigned k;

        if (ask(RAM_OP_LIST, "/many", offset, NULL, 0, 1) != RAM_OK) {
            break;
        }

        for (k = 0; k < said.count; k++) {
            if (strcmp(said.u.entries[k], last) <= 0) {
                ordered = false;
            }

            snprintf(last, sizeof(last), "%s", said.u.entries[k]);
            listed++;
        }

        offset += said.count;

        if (!said.more) {
            break;
        }
    }

    check(listed == 500, "all five hundred listed, a page at a time");
    check(ordered, "listed in order");

    n = get("/many/f123", back, sizeof(back));
    check(n == 8 && memcmp(back, "file 123", 8) == 0, "the 124th read back");

    /* Every one deleted, and the folder: nothing held. */
    for (i = 0; i < 500; i++) {
        snprintf(path, sizeof(path), "/many/f%03u", i);
        (void)ask(RAM_OP_DELETE, path, 0, NULL, 0, 1);
    }

    check(ask(RAM_OP_DELETE, "/many", 0, NULL, 0, 1) == RAM_OK,
          "the folder deleted once it is empty");
    check(ram_store_entries() == 0 && ram_store_held() == 0,
          "after deleting everything, nothing is held");
}

static void big_value(void)
{
    static char data[300u * 1024u], back[300u * 1024u + 1024u];
    uint32_t at;
    size_t i, n;
    bool all = true;

    ram_store_init(64u * 1024u * 1024u);

    for (i = 0; i < sizeof(data); i++) {
        data[i] = (char)((i * 7u + 3u) & 0xffu);
    }

    for (at = 0; at < sizeof(data); at += RAM_DATA_MAX) {
        if (ask(RAM_OP_WRITE, "/big", at, data + at, RAM_DATA_MAX, 1)
            != RAM_OK) {
            all = false;
            break;
        }
    }

    check(all, "300 KB written a message at a time, where 16 KB was the most");

    n = get("/big", back, sizeof(back));
    check(n == sizeof(data) && memcmp(back, data, sizeof(data)) == 0,
          "300 KB read back byte for byte");

    (void)ask(RAM_OP_GETATTR, "/big", 0, NULL, 0, 1);
    check(said.count >= 1 && strcmp(said.u.attrs[0].name, "size") == 0
          && strcmp(said.u.attrs[0].value, "307200") == 0,
          "its size attribute says 307200");
}

static void gap_is_zeros(void)
{
    char back[32];
    size_t n;
    unsigned i;
    bool zeros = true;

    ram_store_init(64u * 1024u * 1024u);

    (void)put("/gap", "abcdefghij");
    (void)put("/gap", "x");                     /* from the start: one byte */
    (void)ask(RAM_OP_WRITE, "/gap", 10, "y", 1, 1);

    n = get("/gap", back, sizeof(back));
    check(n == 11, "a write at 10 after one byte makes eleven");

    for (i = 1; i < 10 && n == 11; i++) {
        if (back[i] != 0) {
            zeros = false;
        }
    }

    check(n == 11 && back[0] == 'x' && back[10] == 'y' && zeros,
          "the gap reads as zeros, not as the bytes written before it");
}

static void ceiling(void)
{
    char chunk[RAM_DATA_MAX], path[64], back[64];
    unsigned files = 0, k;
    uint32_t err = RAM_OK;
    size_t before, n;

    memset(chunk, 'c', sizeof(chunk));
    ram_store_init(256u * 1024u);
    (void)put("/keep", "kept");

    /* Files of 8 KB until the store says no. */
    while (err == RAM_OK && files < 1000) {
        snprintf(path, sizeof(path), "/fill/%u", files);

        for (k = 0; k < 8 && err == RAM_OK; k++) {
            err = ask(RAM_OP_WRITE, path, k * RAM_DATA_MAX, chunk,
                      RAM_DATA_MAX, 1);
        }

        files++;
    }

    check(err == RAM_ERR_FULL, "past the ceiling, a write is refused as full");
    check(ram_store_held() <= 256u * 1024u, "what is held stays under it");
    check(files > 10, "and it held a good deal before it said so");

    n = get("/keep", back, sizeof(back));
    check(n == 4 && memcmp(back, "kept", 4) == 0,
          "what it held before is still there and readable");

    /* A file whose first write is refused is not left behind empty: 200 KB
     * in, which a full store of 256 KB cannot give. */
    before = ram_store_entries();
    check(ask(RAM_OP_WRITE, "/fill/new", 200u * 1024u, chunk, RAM_DATA_MAX, 1)
          == RAM_ERR_FULL, "a write to a new file that cannot fit is refused");
    check(ram_store_entries() == before
          && ask(RAM_OP_GETATTR, "/fill/new", 0, NULL, 0, 1) == RAM_ERR_NO_PATH,
          "and leaves no empty file behind");

    /* Two deleted: their room is back, and a file of the same size fits. */
    before = ram_store_held();
    (void)ask(RAM_OP_DELETE, "/fill/0", 0, NULL, 0, 1);
    (void)ask(RAM_OP_DELETE, "/fill/1", 0, NULL, 0, 1);
    check(ram_store_held() < before, "a delete gives its room back");

    err = RAM_OK;

    for (k = 0; k < 8 && err == RAM_OK; k++) {
        err = ask(RAM_OP_WRITE, "/fill/again", k * RAM_DATA_MAX, chunk,
                  RAM_DATA_MAX, 1);
    }

    check(err == RAM_OK, "and a file of the same size fits again");
}

static void room_given_back(void)
{
    static char chunk[RAM_DATA_MAX];
    uint32_t at;
    size_t big, small;

    memset(chunk, 's', sizeof(chunk));
    ram_store_init(64u * 1024u * 1024u);

    for (at = 0; at < 2u * 1024u * 1024u; at += RAM_DATA_MAX) {
        (void)ask(RAM_OP_WRITE, "/song", at, chunk, RAM_DATA_MAX, 1);
    }

    big = ram_store_held();
    check(big >= 2u * 1024u * 1024u, "a 2 MB value holds 2 MB");

    (void)put("/song", "now a line");
    check(ram_store_held() < 64u * 1024u,
          "written again from the start, a large value gives its room back");

    for (at = 0; at < 10u * 1024u; at += RAM_DATA_MAX) {
        (void)ask(RAM_OP_WRITE, "/status", at, chunk, RAM_DATA_MAX, 1);
    }

    small = ram_store_held();
    (void)put("/status", "ok");
    check(ram_store_held() == small,
          "a small one keeps its room for the next writer");
}

static void many_watches(void)
{
    static struct ram_request req;
    unsigned i, before_told;
    size_t held_before;
    bool all = true;

    ram_store_init(64u * 1024u * 1024u);
    (void)put("/w/a", "a");
    held_before = ram_store_held();

    /* Forty watches for `tag = yes`, each from its own sender, each
     * knowing the answer is nothing - so each is parked. */
    for (i = 0; i < 40; i++) {
        memset(&req, 0, sizeof(req));
        req.op = RAM_OP_WATCH;
        req.offset = 1;                     /* one term */
        req.count = 0;                      /* knowing no paths */
        snprintf(req.path, sizeof(req.path), "/w");
        snprintf(req.attrs[0].name, sizeof(req.attrs[0].name), "tag");
        snprintf(req.attrs[0].value, sizeof(req.attrs[0].value), "yes");

        before_told = ntold;
        ram_answer(&req, sizeof(req), 1000u + i);

        if (ntold != before_told) {
            all = false;
        }
    }

    check(all && ram_store_watches() == 40,
          "forty watches parked at once, where sixteen was the most");

    /* One setattr changes the answer, and every one of them hears. */
    memset(&req, 0, sizeof(req));
    req.op = RAM_OP_SETATTR;
    req.count = 1;
    snprintf(req.path, sizeof(req.path), "/w/a");
    snprintf(req.attrs[0].name, sizeof(req.attrs[0].name), "tag");
    snprintf(req.attrs[0].value, sizeof(req.attrs[0].value), "yes");

    before_told = ntold;
    ram_answer(&req, sizeof(req), 1);

    /* Forty answers and the setattr's own. */
    check(ntold - before_told == 41, "one change answers all forty");
    check(ram_store_watches() == 0, "and none is left parked");

    all = true;

    for (i = before_told; i < before_told + 40 && i < 256; i++) {
        if (told[i] < 1000u || told[i] >= 1040u) {
            all = false;
        }
    }

    check(all, "each answer to one of the forty who asked");
    check(ram_store_held() == held_before,
          "and the forty answered hold nothing any more");
}

static void rename_folder(void)
{
    char back[16];

    ram_store_init(64u * 1024u * 1024u);
    (void)put("/r/a", "first");
    (void)put("/r/b", "second");

    {
        static struct ram_request req;

        memset(&req, 0, sizeof(req));
        req.op = RAM_OP_RENAME;
        snprintf(req.path, sizeof(req.path), "/r");
        snprintf(req.u.data, sizeof(req.u.data), "/s");
        ram_answer(&req, sizeof(req), 1);
    }

    check(said.error == RAM_OK, "a folder renamed");
    check(get("/s/b", back, sizeof(back)) == 6
          && memcmp(back, "second", 6) == 0,
          "with what is in it under the new name");
    check(ask(RAM_OP_GETATTR, "/r/a", 0, NULL, 0, 1) == RAM_ERR_NO_PATH,
          "and nothing left under the old one");
}

static void hostile_offset(void)
{
    ram_store_init(64u * 1024u * 1024u);

    check(ask(RAM_OP_WRITE, "/far", 0xfffffff0u, "0123456789abcdef0123", 20, 1)
          == RAM_ERR_FULL,
          "an offset near the top of 32 bits is refused, not wrapped");
    check(ram_store_held() == 0 && ram_store_entries() == 0,
          "and leaves nothing behind");
}

int main(void)
{
    many_files();
    big_value();
    gap_is_zeros();
    ceiling();
    room_given_back();
    many_watches();
    rename_folder();
    hostile_offset();

    ram_store_init(0);

    if (failed > 0) {
        printf("FAIL: %u of %u checks on /Temporary's store\n", failed, checks);
        return 1;
    }

    printf("PASS: %u checks on /Temporary's store - more than 128 files, "
           "more than 16 KB, a ceiling that refuses, and the room given back\n",
           checks);
    return 0;
}
