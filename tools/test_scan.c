/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The scanf family's scanner, checked on this machine rather than the target.
 *
 * `runtime/libc/scan.c` needs `stdio.h`, `stdlib.h` and `string.h` and
 * nothing of Kosmos's, so the host compiler builds it as the cross one does -
 * with `vsscanf` and `sscanf` renamed on the command line, so that what is
 * called below is Kosmos's scanner and not the host's. `strtol`, `strtoul`
 * and `strtod` are the host's here; what is under test is the scanning
 * around them.
 *
 * Two of these checks are why the scanner moved out of `misc_user.c`: `%f`
 * writes a `float` and not the `double` it used to, and a number is never
 * read past the length the scanner was given - which is what lets `fscanf`
 * read a file inside a pak.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int kosmos_vscan(const char *in, size_t len, const char *fmt, va_list ap,
                 size_t *used);

static int checks;
static int failures;

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        failures++;
        printf("not ok %d - %s\n", checks, what);
    }
}

/* The bounded scanner, the way `fscanf` calls it. */
static int scan(const char *in, size_t len, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = kosmos_vscan(in, len, fmt, ap, used);
    va_end(ap);

    return n;
}

int main(void)
{
    {
        int i = 0;

        check(sscanf("42", "%d", &i) == 1 && i == 42,
              "%d reads a decimal integer");
    }
    {
        int a = 0, b = 0;

        check(sscanf("0x1f 017", "%i %i", &a, &b) == 2 && a == 31 && b == 15,
              "%i takes its base from the prefix");
    }
    {
        unsigned int x = 0, o = 0;

        check(sscanf("ff 17", "%x %o", &x, &o) == 2 && x == 255 && o == 15,
              "%x reads hexadecimal and %o octal");
    }
    {
        struct { float value; float after; } f = { 0.0f, 12345.0f };

        check(sscanf("1.5", "%f", &f.value) == 1 && f.value == 1.5f
              && f.after == 12345.0f,
              "%f stores a float, and nothing past it");
    }
    {
        double d = 0.0;

        check(sscanf("2.25", "%lf", &d) == 1 && d == 2.25,
              "%lf stores a double");
    }
    {
        float v[3] = { 0.0f, 0.0f, 0.0f };

        check(sscanf("1 -2.5 3e2\n", "%f %f %f\n", &v[0], &v[1], &v[2]) == 3
              && v[0] == 1.0f && v[1] == -2.5f && v[2] == 300.0f,
              "a pointfile line: three floats");
    }
    {
        char s[8];

        check(sscanf("abcdef", "%3s", s) == 1 && strcmp(s, "abc") == 0,
              "%3s stops at its width");
    }
    {
        int version = 0;
        char name[80];

        check(sscanf("5\nstart\n", "%i\n%79s\n", &version, name) == 2
              && version == 5 && strcmp(name, "start") == 0,
              "a savegame header: a number, a newline and a word");
    }
    {
        int b = 0;

        check(sscanf("1 2", "%*d %d", &b) == 1 && b == 2,
              "%*d reads a field and stores nothing");
    }
    {
        char c = 0;

        check(sscanf(" x", "%c", &c) == 1 && c == ' ',
              "%c does not skip whitespace");
    }
    {
        int i = 7;

        check(sscanf("", "%d", &i) == EOF && i == 7,
              "empty input is EOF, and nothing is stored");
        check(sscanf("   ", "%d", &i) == EOF,
              "whitespace and nothing else is EOF");
        check(sscanf("x", "%d", &i) == 0 && i == 7,
              "a letter where a number belongs matches nothing");
    }
    {
        short h = 3;

        check(sscanf("9", "%hd", &h) == 0 && h == 3,
              "a length this does not write correctly stops the scan");
    }
    {
        size_t used = 0;
        int a = 0, b = 0;

        check(scan("12 34", 2, &used, "%d %d", &a, &b) == 1
              && a == 12 && b == 0 && used == 2,
              "nothing past the length is read");
    }
    {
        size_t used = 0;
        int n = 0;

        check(scan("123456", 3, &used, "%d", &n) == 1 && n == 123 && used == 3,
              "a number ends at the length, not at the next digit");
    }
    {
        /* A demo: the CD track as text, then the first message - binary, and
         * here beginning with two bytes that happen to be whitespace. */
        static const char demo[] = { '3', '\n', '\n', ' ', 0x01, 0x00 };
        size_t used = 0;
        int track = 0;

        check(scan(demo, sizeof demo, &used, "%i\n", &track) == 1
              && track == 3 && used == 4,
              "a demo header: the track, then every whitespace byte, as C's "
              "fscanf does");
    }
    {
        size_t used = 0;

        check(scan("100% done", 9, &used, "100%% done") == 0 && used == 9,
              "%% and literals are consumed and counted");
    }
    {
        char digits[80];
        int n = 5;

        memset(digits, '7', 70);
        digits[70] = '\0';

        check(sscanf(digits, "%d", &n) == 0 && n == 5,
              "a number longer than the scanner holds is refused, not cut");
    }

    if (failures == 0) {
        printf("PASS: %d checks on the scanf family's scanner, on this "
               "machine.\n", checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on the scanf family's scanner.\n",
           failures, checks);
    return 1;
}
