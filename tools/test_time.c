/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `runtime/libc/time.c` held to the Mac's own C library.
 *
 * The file is compiled here with its four functions renamed (`k_gmtime`
 * and so on, on the compile line in the Makefile), so both are in one
 * program and every answer can be compared with the one the host gives:
 *
 * - `gmtime` over two hundred thousand instants from the year 1653 to 2286,
 *   and the ones that catch calendar arithmetic out - the epoch and the
 *   second before it, 29 February in 2000 and 2400, 1 March 2100 and 1900,
 *   and the first day of the year 1;
 * - `mktime` against the host's `timegm`, which is what a zoneless
 *   `mktime` is, including fields out of range that it has to normalise;
 * - `strftime` in the "C" locale, every conversion it implements, and the
 *   standard's answer of zero when the result does not fit;
 * - and that `localtime` is `gmtime`, which is this system's statement that
 *   it does not know where it is.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

struct tm *k_gmtime(const time_t *t);
struct tm *k_localtime(const time_t *t);
time_t     k_mktime(struct tm *tm);
size_t     k_strftime(char *out, size_t size, const char *format,
                      const struct tm *tm);

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        if (fails <= 20) {
            printf("  not ok: %s\n", what);
        }
    }
}

static int same_fields(const struct tm *a, const struct tm *b)
{
    return a->tm_sec == b->tm_sec && a->tm_min == b->tm_min
        && a->tm_hour == b->tm_hour && a->tm_mday == b->tm_mday
        && a->tm_mon == b->tm_mon && a->tm_year == b->tm_year
        && a->tm_wday == b->tm_wday && a->tm_yday == b->tm_yday
        && a->tm_isdst == b->tm_isdst;
}

static void describe(char *out, size_t n, const char *what, time_t t,
                     const struct tm *k, const struct tm *h)
{
    snprintf(out, n, "%s(%ld): ours %d-%02d-%02d %02d:%02d:%02d wday %d "
             "yday %d, host %d-%02d-%02d %02d:%02d:%02d wday %d yday %d",
             what, (long)t,
             k->tm_year + 1900, k->tm_mon + 1, k->tm_mday, k->tm_hour,
             k->tm_min, k->tm_sec, k->tm_wday, k->tm_yday,
             h->tm_year + 1900, h->tm_mon + 1, h->tm_mday, h->tm_hour,
             h->tm_min, h->tm_sec, h->tm_wday, h->tm_yday);
}

static void one_instant(time_t t)
{
    struct tm host, *ours;
    char what[256];

    gmtime_r(&t, &host);
    ours = k_gmtime(&t);
    if (ours == NULL) {
        snprintf(what, sizeof what, "gmtime(%ld) is null", (long)t);
        check(0, what);
        return;
    }
    describe(what, sizeof what, "gmtime", t, ours, &host);
    check(same_fields(ours, &host), what);
}

static void test_gmtime(void)
{
    static const time_t edges[] = {
        0, -1, 1, 86399, 86400, -86400, -86401,
        951782400,      /* 2000-02-29 */
        951868800,      /* 2000-03-01 */
        13574563200,    /* 2400-02-29 */
        4107542400,     /* 2100-03-01 */
        4107456000,     /* 2100-02-28 */
        -2203891200,    /* 1900-03-01 */
        -62135596800,   /* 0001-01-01 */
        2147483647, 2147483648, -2147483648,
        1790208000,     /* 2026-09-24 */
    };
    time_t t;
    size_t i;

    for (i = 0; i < sizeof edges / sizeof edges[0]; i++) {
        one_instant(edges[i]);
    }
    /* Ten thousand million seconds either side of 1970, a step that is
     * prime so it lands on every hour, weekday and day of the year. */
    for (t = -10000000000; t < 10000000000; t += 99991) {
        one_instant(t);
    }
}

static void test_localtime(void)
{
    time_t t = 1790208000;
    struct tm g = *k_gmtime(&t);

    check(same_fields(k_localtime(&t), &g), "localtime is gmtime");
}

static void one_mktime(struct tm in)
{
    struct tm host = in, ours = in;
    time_t h = timegm(&host);
    time_t k = k_mktime(&ours);
    char what[256];

    host.tm_isdst = 0;
    snprintf(what, sizeof what, "mktime(%d-%d-%d %d:%d:%d) = %ld, "
             "timegm %ld", in.tm_year + 1900, in.tm_mon + 1, in.tm_mday,
             in.tm_hour, in.tm_min, in.tm_sec, (long)k, (long)h);
    check(k == h, what);
    describe(what, sizeof what, "mktime fields", k, &ours, &host);
    check(same_fields(&ours, &host), what);
}

static void test_mktime(void)
{
    static const int cases[][6] = {
        /* year, mon (0-11), mday, hour, min, sec */
        { 1970,  0,   1,   0,   0,   0 },
        { 2026,  8,  24,  12,  34,  56 },
        { 2000,  1,  29,  23,  59,  59 },
        { 1969, 11,  31,  23,  59,  59 },
        { 1900,  2,   1,   0,   0,   0 },
        /* out of range, normalised */
        { 2026,  0,  32,   0,   0,   0 },   /* 32 January */
        { 2026,  0,   0,   0,   0,   0 },   /* 0 January */
        { 2026, -1,  15,   0,   0,   0 },   /* month -1 */
        { 2026, 13,  15,   0,   0,   0 },   /* month 13 */
        { 2026,  5,  15,  25,  61,  -1 },
        { 2024,  1,  30,   0,   0,   0 },   /* 30 February, a leap year */
        { 2100,  1,  29,   0,   0,   0 },   /* 29 February, not one */
        { 1990, -25, 400, -48, 0,   3600 },
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct tm in;

        memset(&in, 0, sizeof in);
        in.tm_year = cases[i][0] - 1900;
        in.tm_mon  = cases[i][1];
        in.tm_mday = cases[i][2];
        in.tm_hour = cases[i][3];
        in.tm_min  = cases[i][4];
        in.tm_sec  = cases[i][5];
        one_mktime(in);
    }
}

static void test_strftime(void)
{
    static const char *const formats[] = {
        "%a %A %b %B %h",
        "%C %d %D %e %F",
        "%H %I %j %m %M %p",
        "%r %R %S %T %u %w",
        "%y %Y %%",
        "%c | %x | %X",
        "%n%t",
        "%Ey %OH",
        "plain text",
        "",
    };
    static const time_t instants[] = {
        0, 1790208000, 951782400, -2203891200, 43200, 46800, 3600,
    };
    size_t i, j;

    for (i = 0; i < sizeof instants / sizeof instants[0]; i++) {
        struct tm tm;

        gmtime_r(&instants[i], &tm);
        for (j = 0; j < sizeof formats / sizeof formats[0]; j++) {
            char host[256], ours[256], what[640];
            size_t hn = strftime(host, sizeof host, formats[j], &tm);
            size_t kn = k_strftime(ours, sizeof ours, formats[j], &tm);

            snprintf(what, sizeof what, "strftime(\"%s\") at %ld: ours "
                     "\"%s\" (%zu), host \"%s\" (%zu)", formats[j],
                     (long)instants[i], ours, kn, host, hn);
            check(kn == hn && strcmp(ours, host) == 0, what);
        }
    }

    {
        time_t t = 1790208000;
        struct tm tm;
        char out[64];

        gmtime_r(&t, &tm);
        check(k_strftime(out, sizeof out, "%z %Z", &tm) == 9
              && strcmp(out, "+0000 UTC") == 0, "strftime %z %Z is UTC");
        /* "2026-09-24" is ten characters and needs eleven with its NUL. */
        check(k_strftime(out, 10, "%F", &tm) == 0,
              "strftime answers zero when the NUL does not fit");
        check(k_strftime(out, 11, "%F", &tm) == 10
              && strcmp(out, "2026-09-24") == 0,
              "strftime fits exactly with its NUL");
        check(k_strftime(out, 0, "%F", &tm) == 0,
              "strftime into nothing answers zero");
    }
}

int main(void)
{
    test_gmtime();
    test_localtime();
    test_mktime();
    test_strftime();

    printf("test_time: %d checks, %d failed\n", checks + fails, fails);
    return fails != 0;
}
