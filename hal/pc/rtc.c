/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The clock the machine keeps while it is off.
 *
 * `hal/qemu-virt/rtc.c` reads a PL031, which is a 32-bit counter of seconds
 * at a fixed address and needs no decoding at all. The PC keeps the same
 * fact in a chip designed in 1984 to hold sixty-four bytes of setup across
 * a power cut, and gets there through two I/O ports: write which byte you
 * want to 0x70, read it from 0x71. The date arrives as six separate
 * numbers, possibly in binary-coded decimal, possibly with the hour in
 * twelve-hour form, and it is the caller's job to notice which.
 *
 * **Two things here look like defensiveness and are not.**
 *
 * The chip updates its registers in place, roughly once a second, and takes
 * about two milliseconds to do it. Reading during that window returns some
 * fields from before the update and some from after - so 01:59:59 becomes
 * 01:00:59 about once an hour, and everything that reads the clock
 * inherits an hour-long error that reproduces on nobody's machine. Status
 * register A bit 7 says an update is in progress, and this waits it out.
 *
 * And it reads the whole date twice and compares, because the update can
 * begin *between* two of the seven port reads below - which the flag cannot
 * tell you about, since it was clear when you looked.
 *
 * MC146818 datasheet, and every PC since has implemented it compatibly.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"
#include "pc.h"

#define CMOS_ADDR   0x70
#define CMOS_DATA   0x71

#define CMOS_SECONDS    0x00
#define CMOS_MINUTES    0x02
#define CMOS_HOURS      0x04
#define CMOS_DAY        0x07
#define CMOS_MONTH      0x08
#define CMOS_YEAR       0x09
#define CMOS_STATUS_A   0x0A
#define CMOS_STATUS_B   0x0B

struct wallclock {
    unsigned second, minute, hour, day, month, year;
};

static uint8_t cmos(uint8_t reg)
{
    /*
     * Bit 7 of the address port is the NMI disable line, and it shares this
     * register with the CMOS index. Writing an index with that bit clear
     * *enables* NMI as a side effect, which is the traditional way to turn
     * something on by accident. Nothing here wants NMI touched either way,
     * and leaving the bit clear is what every other reader of this chip
     * does, so the setting stays wherever the firmware left it only in the
     * sense that nobody else here disturbs it.
     */
    pc_out8(CMOS_ADDR, reg);

    return pc_in8(CMOS_DATA);
}

static bool updating(void)
{
    return (cmos(CMOS_STATUS_A) & 0x80) != 0;
}

static void read_once(struct wallclock *out)
{
    out->second = cmos(CMOS_SECONDS);
    out->minute = cmos(CMOS_MINUTES);
    out->hour   = cmos(CMOS_HOURS);
    out->day    = cmos(CMOS_DAY);
    out->month  = cmos(CMOS_MONTH);
    out->year   = cmos(CMOS_YEAR);
}

static bool same(const struct wallclock *a, const struct wallclock *b)
{
    return a->second == b->second && a->minute == b->minute
        && a->hour   == b->hour   && a->day    == b->day
        && a->month  == b->month  && a->year   == b->year;
}

static unsigned from_bcd(unsigned v)
{
    return (v & 0x0f) + ((v >> 4) * 10);
}

/*
 * Days since 1970-01-01, from a year, month and day.
 *
 * Howard Hinnant's `days_from_civil`, which is the standard way to do this
 * without a table: shift the year to start in March so that the leap day is
 * the last day of it and the month lengths become a linear formula, count
 * whole eras of 400 years, and add the offset within one. 719468 is the
 * number of days from 0000-03-01 to 1970-01-01.
 *
 * Written out rather than approximated because a clock that is right except
 * on the 29th of February is a clock nobody will debug in February.
 */
static unsigned long days_from_civil(unsigned y, unsigned m, unsigned d)
{
    unsigned long era, yoe, doy, doe;

    y -= (m <= 2) ? 1 : 0;
    era = y / 400;
    yoe = y - era * 400;                                    /* 0 to 399 */
    doy = (153u * (m + ((m > 2) ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + doe - 719468;
}

unsigned long hal_rtc_seconds(void)
{
    struct wallclock now, again;
    uint8_t format;
    unsigned tries = 0;

    /*
     * Twice the same, or give up. Bounded because this is the boot path and
     * a chip that never settles must not be a machine that never starts;
     * two reads in a row disagreeing means the update window landed between
     * them, and the next pair will not.
     */
    do {
        while (updating()) {
            /* about two milliseconds, once a second */
        }

        read_once(&now);
        read_once(&again);
    } while (!same(&now, &again) && ++tries < 8);

    format = cmos(CMOS_STATUS_B);

    /*
     * Bit 2 set means the values are plain binary. Clear - which is the
     * default and what QEMU does - means binary-coded decimal, where 0x59
     * is fifty-nine rather than eighty-nine.
     */
    if ((format & 0x04) == 0) {
        bool pm = (now.hour & 0x80) != 0;

        now.second = from_bcd(now.second);
        now.minute = from_bcd(now.minute);
        now.hour   = from_bcd(now.hour & 0x7f);
        now.day    = from_bcd(now.day);
        now.month  = from_bcd(now.month);
        now.year   = from_bcd(now.year);

        now.hour |= pm ? 0x80u : 0u;    /* put the flag back for below */
    }

    /*
     * Bit 1 clear means twelve-hour time, with bit 7 of the hour meaning
     * afternoon - and midnight is 12 rather than 0, so it wraps to zero
     * rather than becoming twelve.
     */
    if ((format & 0x02) == 0) {
        bool pm = (now.hour & 0x80) != 0;

        now.hour &= 0x7f;

        if (now.hour == 12) {
            now.hour = 0;
        }

        if (pm) {
            now.hour += 12;
        }
    }

    /*
     * Two digits, and the century register is not reliable: register 0x32
     * holds it on most machines and something else on some, and ACPI's FADT
     * is where a real system finds out which. Everything this runs on is
     * after 2000 and will be for the life of the project, so the window is
     * 2000 to 2099 and the assumption is written down rather than hidden.
     */
    now.year += 2000;

    return days_from_civil(now.year, now.month, now.day) * 86400UL
         + now.hour * 3600UL + now.minute * 60UL + now.second;
}
