/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Broken-down time: the arithmetic half of <time.h>.
 *
 * There is still no clock in here. `time()` is in `user/init/misc_user.c`
 * and reads the board's; this file only answers which date and hour a
 * count of seconds names, and which count a date names - which needs no
 * clock at all, only the calendar.
 *
 * **FFmpeg asked for it.** Its option parser turns a date into seconds and
 * its logger can stamp a line with the hour, and a decoder links both
 * whether or not anything here ever sets a date option or asks for a stamp.
 * That is a reason to have them correct rather than to have them stubbed:
 * a stub that returns something plausible is found the day something calls
 * it, by whoever is looking at the wrong date. `tools/test_time.c` holds
 * these to the Mac's own libc.
 *
 * **Local time is UTC**, and `localtime` and `mktime` say so by being
 * `gmtime` and its inverse. Nothing on this machine says where it is -
 * there is no zone database and no setting - and a pair that disagreed with
 * each other would be worse than a pair that is honestly zoneless. When a
 * zone arrives it arrives here, in these two, and nowhere else.
 *
 * The day arithmetic is Howard Hinnant's `days_from_civil` and
 * `civil_from_days` (public domain, "chrono-Compatible Low-Level Date
 * Algorithms"): exact over the whole proleptic Gregorian calendar, with no
 * table and no loop over years.
 */

#include <limits.h>
#include <stddef.h>
#include <time.h>

#define SECONDS_PER_DAY 86400L

/* Days from 1970-01-01 to year y, month m (1-12), day d (1-31). */
static long days_from_civil(long y, long m, long d)
{
    long era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                  /* 0-399 */
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1; /* 0-365 */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          /* 0-146096 */
    return era * 146097 + doe - 719468;
}

/* The inverse: the year, month (1-12) and day (1-31) of day z. */
static void civil_from_days(long z, long *y, long *m, long *d)
{
    long era, doe, yoe, doy, mp;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;                                     /* 0-146096 */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* 0-399 */
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              /* 0-365 */
    mp = (5 * doy + 2) / 153;                                   /* 0-11 */
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

/* Floor division, because C's rounds towards zero and 1969 is negative. */
static long floor_div(long a, long b)
{
    return a / b - (a % b != 0 && (a < 0) != (b < 0));
}

static struct tm *broken_down(time_t t, struct tm *out)
{
    long days = floor_div(t, SECONDS_PER_DAY);
    long secs = t - days * SECONDS_PER_DAY;
    long y, m, d;

    civil_from_days(days, &y, &m, &d);

    /* `tm_year` is an int counted from 1900; a year it cannot hold is the
     * one case the standard gives a null pointer for. */
    if (y - 1900 > INT_MAX || y - 1900 < INT_MIN) {
        return NULL;
    }

    out->tm_sec   = (int)(secs % 60);
    out->tm_min   = (int)(secs / 60 % 60);
    out->tm_hour  = (int)(secs / 3600);
    out->tm_mday  = (int)d;
    out->tm_mon   = (int)(m - 1);
    out->tm_year  = (int)(y - 1900);
    /* 1 January 1970 was a Thursday, day 4 counted from Sunday. */
    out->tm_wday  = (int)(days + 4 - floor_div(days + 4, 7) * 7);
    out->tm_yday  = (int)(days - days_from_civil(y, 1, 1));
    out->tm_isdst = 0;
    return out;
}

/*
 * One static result for both, as the standard specifies: each call may
 * overwrite what the last returned. A process that wants two at once
 * copies the first, which is what FFmpeg's `ff_gmtime_r` does.
 */
static struct tm result;

struct tm *gmtime(const time_t *t)
{
    return broken_down(*t, &result);
}

struct tm *localtime(const time_t *t)
{
    return broken_down(*t, &result);
}

/*
 * The inverse, and it normalises: 32 January is 1 February, minute 61 is
 * the next hour, and the fields are written back that way with the day of
 * the week filled in - which is how a caller asks "what day was that".
 */
time_t mktime(struct tm *tm)
{
    long mon = tm->tm_mon;
    long year = (long)tm->tm_year + 1900 + floor_div(mon, 12);
    time_t t;

    mon -= floor_div(mon, 12) * 12;
    t = (days_from_civil(year, mon + 1, 1) + tm->tm_mday - 1) * SECONDS_PER_DAY
        + (long)tm->tm_hour * 3600 + (long)tm->tm_min * 60 + tm->tm_sec;

    if (broken_down(t, tm) == NULL) {
        return (time_t)-1;
    }
    return t;
}

static const char *const day_names[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday",
};

static const char *const month_names[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December",
};

struct sink {
    char  *out;
    size_t size;
    size_t at;
    int    full;
};

static void put(struct sink *s, char c)
{
    if (s->at + 1 < s->size) {
        s->out[s->at++] = c;
    } else {
        s->full = 1;
    }
}

static void puts_n(struct sink *s, const char *text, size_t n)
{
    size_t i;

    for (i = 0; i < n && text[i] != '\0'; i++) {
        put(s, text[i]);
    }
}

static void number(struct sink *s, long v, int width, char pad)
{
    char digits[24];
    int n = 0;
    int negative = v < 0;
    unsigned long u = negative ? 0UL - (unsigned long)v : (unsigned long)v;

    do {
        digits[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);

    if (negative) {
        put(s, '-');
        width--;
    }
    while (n < width--) {
        put(s, pad);
    }
    while (n > 0) {
        put(s, digits[--n]);
    }
}

static void format(struct sink *s, const char *f, const struct tm *tm);

/*
 * C11's conversions (7.27.3.5) in the "C" locale, less the week-based ones
 * - `%G`, `%g`, `%U`, `%V`, `%W` - which nothing has asked for. A field
 * outside its range prints `?` rather than reading outside a table.
 */
static void conversion(struct sink *s, char c, const struct tm *tm)
{
    int wday = tm->tm_wday, mon = tm->tm_mon;
    int valid_wday = wday >= 0 && wday < 7;
    int valid_mon = mon >= 0 && mon < 12;
    int hour12 = tm->tm_hour % 12 == 0 ? 12 : tm->tm_hour % 12;

    switch (c) {
    case 'a': puts_n(s, valid_wday ? day_names[wday] : "?", 3); break;
    case 'A': puts_n(s, valid_wday ? day_names[wday] : "?", 16); break;
    case 'b':
    case 'h': puts_n(s, valid_mon ? month_names[mon] : "?", 3); break;
    case 'B': puts_n(s, valid_mon ? month_names[mon] : "?", 16); break;
    case 'c': format(s, "%a %b %e %H:%M:%S %Y", tm); break;
    case 'C': number(s, ((long)tm->tm_year + 1900) / 100, 2, '0'); break;
    case 'd': number(s, tm->tm_mday, 2, '0'); break;
    case 'D': format(s, "%m/%d/%y", tm); break;
    case 'e': number(s, tm->tm_mday, 2, ' '); break;
    case 'F': format(s, "%Y-%m-%d", tm); break;
    case 'H': number(s, tm->tm_hour, 2, '0'); break;
    case 'I': number(s, hour12, 2, '0'); break;
    case 'j': number(s, tm->tm_yday + 1, 3, '0'); break;
    case 'm': number(s, tm->tm_mon + 1, 2, '0'); break;
    case 'M': number(s, tm->tm_min, 2, '0'); break;
    case 'n': put(s, '\n'); break;
    case 'p': puts_n(s, tm->tm_hour < 12 ? "AM" : "PM", 2); break;
    case 'r': format(s, "%I:%M:%S %p", tm); break;
    case 'R': format(s, "%H:%M", tm); break;
    case 'S': number(s, tm->tm_sec, 2, '0'); break;
    case 't': put(s, '\t'); break;
    case 'T': format(s, "%H:%M:%S", tm); break;
    case 'u': number(s, wday == 0 ? 7 : wday, 1, '0'); break;
    case 'w': number(s, wday, 1, '0'); break;
    case 'x': format(s, "%m/%d/%y", tm); break;
    case 'X': format(s, "%H:%M:%S", tm); break;
    case 'y': number(s, (((long)tm->tm_year + 1900) % 100 + 100) % 100, 2,
                     '0'); break;
    case 'Y': number(s, (long)tm->tm_year + 1900, 1, '0'); break;
    case 'z': puts_n(s, "+0000", 5); break;
    case 'Z': puts_n(s, "UTC", 3); break;
    case '%': put(s, '%'); break;
    default:
        /* Undefined by the standard; shown as written, so it is visible. */
        put(s, '%');
        put(s, c);
        break;
    }
}

static void format(struct sink *s, const char *f, const struct tm *tm)
{
    for (; *f != '\0'; f++) {
        if (*f != '%' || f[1] == '\0') {
            put(s, *f);
            continue;
        }
        f++;
        /* The `E` and `O` modifiers name a locale's alternative forms, and
         * the "C" locale has none, so they are read and dropped. */
        if ((*f == 'E' || *f == 'O') && f[1] != '\0') {
            f++;
        }
        conversion(s, *f, tm);
    }
}

/*
 * The standard's contract, which is odd and worth stating: the count of
 * characters written without the NUL, or zero if they and the NUL did not
 * all fit - in which case what is in `out` is unspecified. A result that is
 * legitimately empty is also zero, and FFmpeg's `av_bprint_strftime` has a
 * comment about exactly that.
 */
size_t strftime(char *out, size_t size, const char *f, const struct tm *tm)
{
    struct sink s = { out, size, 0, 0 };

    if (size == 0) {
        return 0;
    }
    format(&s, f, tm);
    out[s.at] = '\0';
    return s.full ? 0 : s.at;
}
