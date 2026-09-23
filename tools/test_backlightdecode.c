/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * An Intel backlight PWM controller's registers, decoded on the host.
 *
 * QEMU has no Intel graphics, so the backlight driver's only reading under
 * QEMU is that there is nothing to read, and the ThinkPad gives one reading.
 * Everything between is built here: a device that does not answer, a
 * controller that is off, one that is on with numbers that cannot be right,
 * and the widest numbers the registers hold.
 *
 * The layout is Linux's (`backlight_decode.h` says why): enable in bit 31,
 * polarity in bit 29, period and on-time as registers of their own.
 */

#include <stdio.h>

#include "../user/drivers/display/backlight_decode.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

static struct backlight_reading read_of(uint32_t control, uint32_t period,
                                        uint32_t on_time)
{
    struct backlight_controller c = { control, period, on_time };
    struct backlight_reading r;

    backlight_decode(&c, &r);
    return r;
}

int main(void)
{
    struct backlight_reading r;

    /* Nothing decodes the address: every bit of all three reads as one. */
    r = read_of(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    check(r.state == BACKLIGHT_NOT_ANSWERING,
          "all ones was not read as a device that does not answer - it "
          "would pass for enabled, inverted and exactly full");

    /* Off: bit 31 clear, whatever the rest holds. */
    r = read_of(0x00000000u, 1000u, 500u);
    check(r.state == BACKLIGHT_OFF && r.duty_percent == 0,
          "a controller with bit 31 clear was not read as off");

    r = read_of(0x20000000u, 1000u, 500u);
    check(r.state == BACKLIGHT_OFF && r.active_low,
          "an off controller lost its polarity bit");

    /* On, and sensible: a quarter of each period. */
    r = read_of(0x80000000u, 1000u, 250u);
    check(r.state == BACKLIGHT_ON && r.duty_percent == 25 && !r.active_low,
          "on, 250 of 1000, was not a quarter with normal polarity");

    /* On, inverted. */
    r = read_of(0xA0000000u, 1000u, 250u);
    check(r.state == BACKLIGHT_ON && r.active_low && r.duty_percent == 25,
          "bit 29 was not read as inverted polarity");

    /* On, and dark: an on-time of nought is a real setting, not an error. */
    r = read_of(0x80000000u, 1000u, 0u);
    check(r.state == BACKLIGHT_ON && r.duty_percent == 0,
          "an on-time of nought was not read as on and dark");

    /* On, and full. */
    r = read_of(0x80000000u, 1000u, 1000u);
    check(r.state == BACKLIGHT_ON && r.duty_percent == 100,
          "an on-time equal to the period was not full");

    /* Rounded down, never up: 999 of 1000 is not yet all of it. */
    r = read_of(0x80000000u, 1000u, 999u);
    check(r.state == BACKLIGHT_ON && r.duty_percent == 99,
          "999 of 1000 did not round down to 99");

    /* On, with numbers that cannot be right. */
    r = read_of(0x80000000u, 0u, 0u);
    check(r.state == BACKLIGHT_INCONSISTENT,
          "an enabled controller with no period was believed");

    r = read_of(0x80000000u, 1000u, 1001u);
    check(r.state == BACKLIGHT_INCONSISTENT,
          "an on-time past its period was believed");

    /* The widest: a hundred times this does not fit in thirty-two bits,
     * and the period is exactly twice it, so the answer is exactly half. */
    r = read_of(0x80000000u, 0xFFFFFFFEu, 0x7FFFFFFFu);
    check(r.state == BACKLIGHT_ON && r.duty_percent == 50,
          "a period near 2^32 overflowed the arithmetic");

    r = read_of(0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFFu);
    check(r.state == BACKLIGHT_ON && r.duty_percent == 100,
          "a period and on-time of all ones with control not all ones was "
          "not read as full");

    /* The bits Linux calls reserved do not change the answer. */
    r = read_of(0x9FFFFFFFu, 400u, 100u);
    check(r.state == BACKLIGHT_ON && !r.active_low && r.duty_percent == 25,
          "bits other than 31 and 29 changed the reading");

    /* The write at boot, starting from the ThinkPad's own reading on 18
     * September: controller 0 on, period 19393, on-time 6464 - a third. */
    {
        struct backlight_controller t14 = { 0x80000000u, 19393u, 6464u };
        struct backlight_controller c;
        uint32_t on = 0;

        check(backlight_boot_on_time(&t14, 80, &on) && on == 15514u,
              "the ThinkPad's third was not raised to 15514 of 19393, 80%");

        on = 0;
        check(backlight_boot_on_time(&t14, 250, &on) && on == 19393u,
              "a target past 100% was not held to the period");

        c = t14;
        c.on_time = 17000u;             /* already brighter than 80% */
        check(!backlight_boot_on_time(&c, 80, &on),
              "a screen already brighter than the target was dimmed");

        c = t14;
        c.on_time = 15514u;             /* exactly the target */
        check(!backlight_boot_on_time(&c, 80, &on),
              "a screen already at the target was written again");

        c = t14;
        c.control = 0;                  /* off */
        check(!backlight_boot_on_time(&c, 80, &on),
              "a controller that is off was written");

        c = t14;
        c.on_time = 20000u;             /* past its period */
        check(!backlight_boot_on_time(&c, 80, &on),
              "a controller whose numbers do not hang together was written");

        c.control = c.period = c.on_time = 0xFFFFFFFFu;
        check(!backlight_boot_on_time(&c, 80, &on),
              "a device that does not answer was written");
    }

    /* The keys' levels, 0 to 256, on the ThinkPad's period. */
    {
        struct backlight_controller c = { 0x80000000u, 19393u, 15514u };
        uint32_t on = 0;
        unsigned level;
        int drifted = 0;

        check(backlight_level(&c) == 205u,
              "the T14 at 80% (15514 of 19393) did not read as level 205");

        /* Every level from the floor up survives being written and read:
         * rounded down, 204 went in and 203 came out. */
        for (level = 16; level <= 256; level++) {
            c.on_time = 0;
            if (!backlight_on_time_for(&c, level, 16, &on)) {
                drifted++;
                continue;
            }
            c.on_time = on;
            if (backlight_level(&c) != level) {
                drifted++;
            }
        }
        check(drifted == 0, "a level did not read back as the level set");

        c.on_time = 15514u;
        check(backlight_on_time_for(&c, 0, 16, &on)
              && on == (19393u * 16u + 128u) / 256u,
              "a level of 0 was not raised to the floor - the panel would "
              "have gone black");

        check(backlight_on_time_for(&c, 999, 16, &on) && on == 19393u,
              "a level past 256 was not held to the whole period");

        c.control = 0;
        check(!backlight_on_time_for(&c, 128, 16, &on)
              && backlight_level(&c) == 0,
              "a controller that is off was given a level or an on-time");

        c.control = 0x80000000u;
        c.on_time = 20000u;
        check(!backlight_on_time_for(&c, 128, 16, &on),
              "a controller whose numbers do not hang together was given "
              "an on-time");
    }

    if (fails) {
        printf("FAIL: %d of %d checks on the backlight's registers\n",
               fails, fails + checks);
        return 1;
    }

    printf("PASS: %d checks on the backlight's registers (a device that does "
           "not answer, off, on in either polarity from dark to full, "
           "rounded down, numbers that cannot be right refused, periods "
           "near 2^32 without overflow, and the write at boot: the "
           "ThinkPad's third raised to 80%%, never a dimmer screen, never a "
           "controller that is off or wrong; and the keys' levels, each read "
           "back as it was set, never below the floor).\n", checks);
    return 0;
}
