/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A name inside an SMB share, on the Mac (`user/servers/smbpath.c`,
 * `docs/sharing.md` N12b): backslashes and slashes both, the share's root
 * for an empty name, and refused - "." and "..", wherever they stand and
 * however they are written, a stream's ":", and a path longer than the
 * room for it. Names that only look like a climb ("...", "..x", ".hidden")
 * are names.
 */

#include "smbpath.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;

static void is(const char *name, const char *want)
{
    char out[64];
    bool ok = smb_share_path("/Home/Projects", name, out, sizeof out);

    checks++;

    if (want == NULL ? ok : (!ok || strcmp(out, want) != 0)) {
        fails++;
        printf("  %-28s gave %s, wanted %s\n", name != NULL ? name : "(none)",
               ok ? out : "a refusal", want != NULL ? want : "a refusal");
    }
}

int main(void)
{
    char tiny[20];

    is(NULL, "/Home/Projects");
    is("", "/Home/Projects");
    is("hello.txt", "/Home/Projects/hello.txt");
    is("Sub\\inner.txt", "/Home/Projects/Sub/inner.txt");
    is("\\Sub\\inner.txt", "/Home/Projects/Sub/inner.txt");
    is("Sub/inner.txt", "/Home/Projects/Sub/inner.txt");
    is("Sub\\\\inner.txt\\", "/Home/Projects/Sub/inner.txt");
    is("...", "/Home/Projects/...");
    is("..x", "/Home/Projects/..x");
    is(".hidden", "/Home/Projects/.hidden");

    is("..", NULL);
    is("..\\Private\\secret.txt", NULL);
    is("../Private/secret.txt", NULL);
    is("Sub\\..\\..\\Private", NULL);
    is("Sub\\..", NULL);
    is(".", NULL);
    is("Sub\\.\\inner.txt", NULL);
    is("hello.txt:Zone.Identifier", NULL);
    is("a-name-long-enough-to-pass-the-room-there-is-for-it", NULL);

    checks++;
    if (smb_share_path("/Home/Projects", "", tiny, 10)) {
        fails++;
        printf("  a root longer than the room was not refused\n");
    }

    if (fails > 0) {
        printf("FAIL: %d of %d checks on SMB share paths\n", fails, checks);
        return 1;
    }

    printf("PASS: %d checks on SMB share paths (backslashes and slashes, the root, "
           "\"..\" and \".\" anywhere refused, a stream refused, a long path refused, "
           "names that only look like a climb kept)\n", checks);
    return 0;
}
