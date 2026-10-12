/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `user/servers/progcount.c` on the host: the table of which programs read
 * and wrote a drive (`.programs`, Disk Monitor's D2), as the four servers
 * behind a drive keep it.
 *
 * Usage: test_progcount
 */
#include <stdio.h>
#include <string.h>

#include "progcount.h"

static int checks, fails;

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        fails++;
        printf("  %s\n", what);
    }
}

int main(void)
{
    struct progcount t = { 0 };
    struct disk_program page[DEVICE_PROGRAMS_PAGE];
    uint32_t more = 0;
    unsigned n, seen = 0, from = 0;
    char long_path[200];

    /* One program, its reads and writes kept apart. */
    progcount_note(&t, 40, false, 3000, "/Photos/a.jpg");
    progcount_note(&t, 40, false, 1000, "/Photos/b.jpg");
    progcount_note(&t, 40, true, 500, "/Notes.md");
    n = progcount_page(&t, 0, page, DEVICE_PROGRAMS_PAGE, &more);
    check(n == 1 && page[0].process == 40, "one program is one row");
    check(page[0].reads == 2 && page[0].read_bytes == 4000, "its reads were not 2 of 4000 bytes");
    check(page[0].writes == 1 && page[0].write_bytes == 500, "its write was not 1 of 500 bytes");
    check(strcmp(page[0].last, "/Notes.md") == 0, "its last path was not the write's");
    check(more == 0, "one row said more followed");

    /* Twenty programs: more than a page, and more than the table's first
     * room - grown, and paged without a row lost or seen twice. */
    for (uint32_t p = 100; p < 120; p++) {
        progcount_note(&t, p, (p & 1u) != 0, p, "/x");
    }

    do {
        n = progcount_page(&t, from, page, DEVICE_PROGRAMS_PAGE, &more);
        seen += n;
        from += n;
    } while (more && n > 0);

    check(seen == 21, "twenty-one programs did not page as twenty-one rows");
    check(t.room >= 21, "the table did not grow past its first room");

    /* A path longer than the row keeps its end - the file's name. */
    memset(long_path, 'd', sizeof(long_path));
    memcpy(long_path + sizeof(long_path) - 12, "/the-end.txt", 12);
    long_path[sizeof(long_path) - 1] = '\0';
    progcount_note(&t, 40, false, 1, long_path);
    n = progcount_page(&t, 0, page, 1, &more);
    check(strlen(page[0].last) == DEVICE_LAST_MAX - 1u
          && strcmp(page[0].last + strlen(page[0].last) - 11, "/the-end.tx") == 0,
          "a long path did not keep its end");

    if (fails == 0) {
        printf("PASS: %d checks on progcount (a program's reads and writes kept apart, "
               "its last path; twenty-one programs grown into and paged whole; a long "
               "path cut keeping its end)\n", checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on progcount\n", fails, checks);
    return 1;
}
