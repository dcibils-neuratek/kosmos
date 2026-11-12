/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The POSIX layer's `glob` and `fnmatch`, held to this Mac's**
 * (`runtime/posix/glob.c`): every pattern in a table against every name
 * and every flag, the answers compared one by one; then a folder tree made
 * here, globbed both ways, the lists compared name by name.
 *
 * Kosmos's are compiled in under other names, against this Mac's headers,
 * so the two run side by side in one program. `KOSMOS_GLOB_BREAK=1`
 * makes Kosmos's `fnmatch` forget `FNM_PERIOD`, which this must catch.
 */

/* This Mac's own, declared under their own names first; Kosmos's then
 * include the same headers and find them already read. */
#include <fnmatch.h>
#include <glob.h>

#define glob     kosmos_glob
#define globfree kosmos_globfree
#define fnmatch  kosmos_fnmatch
#include "../runtime/posix/glob.c"
#undef glob
#undef globfree
#undef fnmatch

#include <stdio.h>
#include <unistd.h>

static const char *patterns[] = {
    "*", "?", "a*", "*a", "*.xml", "a?c", "[abc]", "[!abc]", "[^abc]", "[a-c]x",
    "[]]", "[!]]", "[a-]", "[", "a[", "\\*", "\\a", "a\\", "*/*", "a/*", "a*/b",
    "*b*", "**", ".*", "*.*", "?x", "a/?", "a/.b", "a/*b", "[.]x", "x[/]y",
    "*[xy]", "[[]", "a]b", "[\\]]", "[a\\-c]", "*?*", "", "x*y*z", "a/b/c",
};

static const char *names[] = {
    "", "a", "b", "abc", "a.xml", ".xml", "x.xml", ".a", "a/b", "a/.b", "a/xb",
    "ab/b", "]", "[", "a[", "*", "\\", "a\\", "-", "bx", ".x", "x/y", "xay",
    "xyz", "xaybz", "a/b/c", "a//b", "abcx", "ax", "cx", "dx",
};

static const int flags[] = {
    0, FNM_PATHNAME, FNM_PERIOD, FNM_NOESCAPE, FNM_PATHNAME | FNM_PERIOD,
    FNM_PATHNAME | FNM_NOESCAPE | FNM_PERIOD,
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static int failures;

/* Where POSIX and this Mac part: an unclosed `[` is an ordinary character
 * to POSIX and to glibc, and BSD's libc refuses the pattern. These, and
 * only these, are to differ. */
static int unclosed(const char *pattern)
{
    return strcmp(pattern, "[") == 0 || strcmp(pattern, "a[") == 0;
}

static void make(const char *path)
{
    FILE *f;

    if (path[strlen(path) - 1] == '/') {
        mkdir(path, 0755);
        return;
    }

    if ((f = fopen(path, "w")) != NULL) {
        fputs("x", f);
        fclose(f);
    }
}

static void both(const char *pattern, int fl)
{
    glob_t mine, theirs;
    int a, b;
    size_t i;

    memset(&mine, 0, sizeof mine);
    memset(&theirs, 0, sizeof theirs);
    a = kosmos_glob(pattern, fl, NULL, &mine);
    b = glob(pattern, fl, NULL, &theirs);

    if (a != b || mine.gl_pathc != theirs.gl_pathc) {
        printf("FAIL: glob \"%s\" flags %x: %d with %zu names, the Mac %d with %zu\n",
               pattern, fl, a, mine.gl_pathc, b, theirs.gl_pathc);
        failures++;
    } else {
        for (i = 0; i < mine.gl_pathc; i++) {
            if (strcmp(mine.gl_pathv[i], theirs.gl_pathv[i]) != 0) {
                printf("FAIL: glob \"%s\" flags %x: name %zu is %s, the Mac's %s\n",
                       pattern, fl, i, mine.gl_pathv[i], theirs.gl_pathv[i]);
                failures++;
                break;
            }
        }
    }

    if (a == 0 || (fl & GLOB_NOCHECK)) {
        kosmos_globfree(&mine);
    }

    globfree(&theirs);
}

int main(void)
{
    static const char *tree[] = {
        "a.xml", "b.xml", ".hidden.xml", "c.txt", "br[ack]et", "sub/", "sub/c.xml",
        "sub/d.txt", "sub/.e.xml", "sub2/", "sub2/e.xml", "sub2/deep/", "sub2/deep/f.xml",
    };
    static const char *globs[] = {
        "*.xml", "*/*.xml", "s*/*", "sub?/*", "[ab].xml", "nothing*", "sub/c.xml",
        "sub/none.xml", ".*.xml", "*", "*/", "./*.xml", "./sub/*.txt", "*/*/*.xml",
        "sub*/d*", "br\\[ack]et", "br[[]ack]et", "sub/.*", "c.txt/*",
    };
    static const int glob_flags[] = { 0, GLOB_MARK, GLOB_NOCHECK, GLOB_NOSORT };
    char dir[] = "/tmp/kosmos-glob-XXXXXX";
    char here[1024];
    size_t p, n, f;
    unsigned compared = 0;
    int broken = getenv("KOSMOS_GLOB_BREAK") != NULL;

    for (p = 0; p < COUNT(patterns); p++) {
        for (n = 0; n < COUNT(names); n++) {
            for (f = 0; f < COUNT(flags); f++) {
                int fl = flags[f];
                int mine = kosmos_fnmatch(patterns[p], names[n],
                                          broken ? fl & ~FNM_PERIOD : fl) == 0;
                int theirs = fnmatch(patterns[p], names[n], fl) == 0;

                compared++;

                if (unclosed(patterns[p]) && !broken) {
                    if (mine != (strcmp(patterns[p], names[n]) == 0)) {
                        printf("FAIL: fnmatch(\"%s\", \"%s\", %x) does not take the "
                               "[ as itself\n", patterns[p], names[n], fl);
                        failures++;
                    }
                    continue;
                }

                if (mine != theirs) {
                    printf("FAIL: fnmatch(\"%s\", \"%s\", %x) is %s, the Mac says %s\n",
                           patterns[p], names[n], fl, mine ? "a match" : "no match",
                           theirs ? "a match" : "no match");
                    failures++;
                }
            }
        }
    }

    if (mkdtemp(dir) == NULL || getcwd(here, sizeof here) == NULL || chdir(dir) != 0) {
        printf("FAIL: no folder to glob in\n");
        return 1;
    }

    for (n = 0; n < COUNT(tree); n++) {
        make(tree[n]);
    }

    for (p = 0; p < COUNT(globs); p++) {
        for (f = 0; f < COUNT(glob_flags); f++) {
            if (glob_flags[f] == GLOB_NOSORT) {
                continue;               /* in no order to compare */
            }

            both(globs[p], glob_flags[f]);
            compared++;
        }
    }

    for (n = COUNT(tree); n-- > 0;) {
        (tree[n][strlen(tree[n]) - 1] == '/' ? rmdir : unlink)(tree[n]);
    }

    if (chdir(here) != 0 || rmdir(dir) != 0) {
        printf("FAIL: the folder %s was not taken away\n", dir);
        failures++;
    }

    if (failures != 0) {
        printf("FAIL: %d of %u answers differ from this Mac's\n", failures, compared);
        return 1;
    }

    printf("glob: %u answers, each this Mac's, or POSIX's where they part\n", compared);
    return 0;
}
