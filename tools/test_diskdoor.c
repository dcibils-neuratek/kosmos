/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The disk server's doors, on the Mac (`user/servers/diskdoor.c`,
 * `docs/maps.md` M6d): the map's cache door, `Home/Cache/Maps`, reaching
 * its folder and what is in it, whatever the case; refused a folder beside
 * it, a name that only begins like it, and any climb out through "." or
 * ".."; allowed to make the folders above it and nothing else above; a
 * rename's destination held to the same; and the disk itself refused to
 * every door but the first. The keyring's door, one folder deep, alongside.
 */

#include "diskdoor.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

static int asks(const char *root, int restricted, unsigned op, const char *path, const char *to)
{
    static struct disk_request rq;

    memset(&rq, 0, sizeof rq);
    rq.op = op;
    snprintf(rq.path, sizeof rq.path, "%s", path);

    if (to != NULL) {
        snprintf(rq.u.to, sizeof rq.u.to, "%s", to);
        rq.length = (unsigned)strlen(to);
    }

    return disk_door_allows(&rq, root, restricted != 0);
}

int main(void)
{
    const char *maps = "Home/Cache/Maps";
    char said[300];

    static const char *in[] = {
        "/Home/Cache/Maps", "/Home/Cache/Maps/", "Home/Cache/Maps/0123456789abcdef",
        "/Home/Cache/Maps/0123456789abcdef/14/8192-8191.pbf", "//Home//Cache/Maps//x",
        "/home/cache/maps/a", "/Home/Cache/Maps/a.b/..c/c..",
    };
    static const char *out[] = {
        "/Home", "/Home/Cache", "/Home/Cache/Map", "/Home/Cache/Mapsx/a", "/Home/Cache/Photos/a",
        "/Home/Cache/Maps/../Photos/a", "/Home/Cache/Maps/a/../../Photos",
        "/Home/Cache/Maps/..", "/Home/Cache/Maps/./a", "/Home/Cache/Maps/a/.",
        "/Keyring/k", "/", "", "/Home/Cache/../Cache/Maps/a", "/Home/./Cache/Maps/a",
    };

    for (size_t i = 0; i < sizeof in / sizeof in[0]; i++) {
        snprintf(said, sizeof said, "%s was not let through the map's cache door", in[i]);
        check(asks(maps, 1, DISK_OP_WRITE, in[i], NULL), said);
        check(asks(maps, 1, DISK_OP_READ, in[i], NULL), said);
    }

    for (size_t i = 0; i < sizeof out / sizeof out[0]; i++) {
        snprintf(said, sizeof said, "%s was let through the map's cache door", out[i]);
        check(!asks(maps, 1, DISK_OP_WRITE, out[i], NULL), said);
        check(!asks(maps, 1, DISK_OP_DELETE, out[i], NULL), said);
    }

    /* The folders above: made, and nothing else done to them. */
    check(asks(maps, 1, DISK_OP_MKDIR, "/Home", NULL), "/Home could not be made through the door");
    check(asks(maps, 1, DISK_OP_MKDIR, "/Home/Cache/", NULL),
          "/Home/Cache could not be made through the door");
    check(!asks(maps, 1, DISK_OP_MKDIR, "/Home/Cache/Photos", NULL),
          "a folder beside the door's could be made through it");
    check(!asks(maps, 1, DISK_OP_MKDIR, "/Home/Cach", NULL),
          "a folder that only begins like one above could be made");
    check(!asks(maps, 1, DISK_OP_DELETE, "/Home/Cache", NULL),
          "a folder above the door's could be deleted through it");
    check(!asks(maps, 1, DISK_OP_LIST, "/Home", NULL),
          "a folder above the door's could be listed through it");

    /* A rename: to a name, or to a path that is the door's too. */
    check(asks(maps, 1, DISK_OP_RENAME, "/Home/Cache/Maps/k/1/2-3.tmp", "2-3.pbf"),
          "a rename to a name in the door's folder was refused");
    check(asks(maps, 1, DISK_OP_RENAME, "/Home/Cache/Maps/a", "/Home/Cache/Maps/b"),
          "a rename inside the door's folder was refused");
    check(!asks(maps, 1, DISK_OP_RENAME, "/Home/Cache/Maps/a", "/Home/Photos/a"),
          "a rename out of the door's folder was let through");
    check(!asks(maps, 1, DISK_OP_RENAME, "/Home/Cache/Maps/a", "/Home/Cache/Maps/../a"),
          "a rename climbing out by .. was let through");

    /* The disk itself: the first door's alone. */
    check(!asks(maps, 1, DISK_OP_SUPER, "", NULL), "the map's door could ask for the disk's super");
    check(!asks(maps, 1, DISK_OP_FORMAT, "", NULL), "the map's door could format the disk");
    check(asks("Home", 0, DISK_OP_SUPER, "", NULL), "the first door could not ask for the super");

    /* The keyring's door, one folder deep, as before. */
    check(asks("Keyring", 1, DISK_OP_WRITE, "/Keyring/keys", NULL),
          "the keyring's door did not reach /Keyring/keys");
    check(!asks("Keyring", 1, DISK_OP_READ, "/Home/x", NULL), "the keyring's door reached /Home");
    check(!asks("Keyring", 1, DISK_OP_READ, "/Keyring/../Home/x", NULL),
          "the keyring's door climbed out by ..");

    if (fails == 0) {
        printf("PASS: %d checks on the disk server's doors (the map's cache door: its folder "
               "whatever the case, nothing beside or out through . or .., the folders above "
               "made and nothing else, a rename held to it, the disk refused; the keyring's)\n",
               checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on the disk server's doors\n", fails, checks + fails);
    return 1;
}
