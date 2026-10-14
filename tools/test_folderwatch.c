/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `user/servers/folderwatch.c` on the Mac (`docs/pulses.md` P4): which
 * watcher a change to a path tells, and which it does not - the folder it
 * is in and no other, whatever slashes the paths were written with; the
 * same owner and code moving a watch rather than adding one; a watch taken
 * back by its owner only; and a subscriber that is gone let go. The
 * kernel's pulse and drop are stood in for.
 */

#include "folderwatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("not ok - %s\n", what);
    }
}

static long pulsed_cap[64];
static uint32_t pulsed_code[64];
static int pulses;
static long dropped[64];
static int drops;

static long fake_pulse(long cap, uint32_t code, uint64_t value)
{
    (void)value;

    if (cap == 99) {
        return -1;                  /* a subscriber gone */
    }

    pulsed_cap[pulses] = cap;
    pulsed_code[pulses] = code;
    pulses++;
    return 0;
}

static void fake_drop(long cap)
{
    dropped[drops++] = cap;
}

int main(void)
{
    struct folderwatch fw = { 0 };

    fw.pulse = fake_pulse;
    fw.drop = fake_drop;
    fw.grow = realloc;

    check(folderwatch_add(&fw, "/Home/Documents/", 5, 100, 7), "a watch was refused");
    check(folderwatch_add(&fw, "Home/Pictures", 6, 101, 7), "a second watch was refused");
    check(folderwatch_add(&fw, "Home", 8, 102, 9), "another owner's watch was refused");

    /* The folder a path is in, and no other. */
    pulses = 0;
    check(folderwatch_changed(&fw, "Home/Documents/a.txt") == 1 && pulses == 1
          && pulsed_cap[0] == 5 && pulsed_code[0] == 100,
          "a file in Documents did not tell Documents' watcher, alone");

    pulses = 0;
    check(folderwatch_changed(&fw, "/Home/Documents/Letters/b.txt") == 0 && pulses == 0,
          "a file a folder deeper told Documents' watcher");

    pulses = 0;
    check(folderwatch_changed(&fw, "/Home/Pictures") == 1 && pulsed_code[0] == 102,
          "a folder made in Home did not tell Home's watcher");

    /* The same owner and code moves the watch, and lets the old door go. */
    drops = 0;
    check(folderwatch_add(&fw, "Home/Music", 11, 100, 7) && fw.used == 3,
          "the same owner and code added a watch instead of moving it");
    check(drops == 1 && dropped[0] == 5, "the moved watch's old capability was not let go");
    pulses = 0;
    check(folderwatch_changed(&fw, "Home/Documents/a.txt") == 0,
          "the folder a watch moved away from still told it");
    check(folderwatch_changed(&fw, "Home/Music/song.mp3") == 1, "the folder it moved to did not");

    /* Taken back by its owner only. */
    folderwatch_remove(&fw, 101, 9);
    check(fw.used == 3, "another owner took a watch back");
    drops = 0;
    folderwatch_remove(&fw, 101, 7);
    check(fw.used == 2 && drops == 1 && dropped[0] == 6, "its owner did not take the watch back");

    /* A subscriber gone is let go at its first change. */
    check(folderwatch_add(&fw, "Home/Gone", 99, 200, 7), "the gone one's watch was refused");
    drops = 0;
    check(folderwatch_changed(&fw, "Home/Gone/x") == 0 && fw.used == 2 && drops == 1,
          "a subscriber that is gone was kept");

    /* It grows: a hundred watches. */
    for (unsigned i = 0; i < 100; i++) {
        char dir[32];

        snprintf(dir, sizeof dir, "Home/F%u", i);
        folderwatch_add(&fw, dir, 1000 + (long)i, 300 + i, 7);
    }

    check(fw.used == 102, "a hundred watches were not all kept");

    free(fw.list);

    if (fails == 0) {
        printf("PASS: %d checks on folderwatch (the folder a change is in told and no other, "
               "whatever its slashes; a watch moved by its owner and code, its old door let go; "
               "taken back by its owner only; a gone subscriber let go; a hundred kept)\n", checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on folderwatch\n", fails, checks);
    return 1;
}
