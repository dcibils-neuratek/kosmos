/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `user/include/depth.h` on the Mac: how deep the Synth Kit keeps its ring
 * and the audio server the device (`roadmap.md` 4i, `testing.md` 18.269).
 *
 *   build/host/test_depth
 *
 * Scripted queues with a counter of 1000 a second and a window of 250:
 *
 *   - it starts full;
 *   - with two or more queued at every look it comes down one a window, to
 *     the floor it was given and no further;
 *   - not inside a window: a step down needs the whole window;
 *   - with one queued at some look it does not come down that window;
 *   - a look that finds it empty is a dry run: one more at once, and that
 *     depth a floor it never comes below again;
 *   - never above the queue's size;
 *   - a window after `depth_resume` is a whole window again, however long
 *     nobody looked: never judged on one look;
 *   - and a queue that asks for a bite of one - the audio server's device -
 *     comes down while it is taken a period at a time, to two, and not while
 *     it is taken two at a time, however much is spare; nor on a queue
 *     holding more than it keeps, which is where it is just after a step.
 */
#include <stdio.h>

#include "../user/include/depth.h"

static int failed, checked;

static void check(int ok, const char *what)
{
    checked++;

    if (!ok) {
        failed++;
        printf("  FAIL: %s\n", what);
    }
}

/* Looks every 4 ms for `ms`, each finding `kept - taken` queued. */
static uint64_t play(struct depth *d, uint64_t now, unsigned ms, uint32_t taken)
{
    for (unsigned t = 0; t < ms; t += 4) {
        uint32_t q = d->kept > taken ? d->kept - taken : 0;

        depth_look(d, q, now, 250);
        now += 4;
    }

    return now;
}

int main(void)
{
    struct depth d;
    uint64_t now = 1000;

    depth_begin(&d, 8, 2, 0, now);
    check(d.kept == 8 && d.floor == 2 && d.dry == 0, "it does not start full");

    /* One taken between looks: seven, then fewer, always two or more. */
    now = play(&d, now, 240, 1);
    check(d.kept == 8, "it came down inside its first window");
    now = play(&d, now, 20, 1);
    check(d.kept == 7, "a whole window with two to spare did not take one away");

    now = play(&d, now, 2000, 1);
    check(d.kept == 2, "it did not come down to its floor of two");
    now = play(&d, now, 2000, 1);
    check(d.kept == 2 && d.dry == 0, "it went below its floor, or ran dry");

    /* A window with one queued at a look: it stays. */
    depth_begin(&d, 8, 1, 0, now);
    now = play(&d, now, 3000, 1);
    check(d.kept == 2 && d.dry == 0,
          "with one spare it came below two, which leaves one - and ran dry");

    /* A dry run at three: four, and never three again. */
    depth_begin(&d, 8, 1, 0, now);
    now = play(&d, now, 1300, 1);             /* five windows: 8 to 3 */
    check(d.kept == 3, "it did not reach three to be tested there");
    depth_look(&d, 0, now, 250);
    check(d.kept == 4 && d.floor == 4 && d.dry == 1,
          "a dry run did not raise it one and make that its floor");
    now = play(&d, now + 4, 3000, 1);
    check(d.kept == 4, "it came back below the depth that ran dry");

    /* Dry at the top: it stays at the queue's size. */
    depth_begin(&d, 4, 1, 0, now);
    depth_look(&d, 0, now, 250);
    check(d.kept == 4 && d.floor == 4, "a dry run took it past the queue's size");

    /* A floor asked for above the size is the size. */
    depth_begin(&d, 4, 9, 0, now);
    check(d.floor == 4 && d.kept == 4, "a floor above the size was kept");

    /* Steady: one taken between looks, a bite of one allowed. Down to two -
     * where a look finds one, and one is not a spare - and no further. */
    depth_begin(&d, 4, 1, 1, now);
    now = play(&d, now, 3000, 1);
    check(d.kept == 2 && d.dry == 0,
          "a queue taken a period at a time did not come down to two");

    /* Bursts: two taken between looks. The old rule came down to three on
     * these, and the first bite of three emptied it. */
    depth_begin(&d, 4, 1, 1, now);
    now = play(&d, now, 3000, 2);
    check(d.kept == 4, "a queue taken two at a time came down");

    /* The same bursts, with any bite allowed, as the Synth Kit's ring: it
     * does come down to three, which is why the device asks. */
    depth_begin(&d, 4, 1, 0, now);
    now = play(&d, now, 3000, 2);
    check(d.kept == 3, "with any bite allowed, two-period bites did not "
                       "bring it to three");

    /* Nobody looked for five seconds - nothing was playing - and then a
     * stream fills it. The first look must not end a window timed from
     * before the pause. */
    depth_begin(&d, 4, 1, 1, now);
    now += 5000;
    depth_resume(&d, now);
    depth_look(&d, 3, now + 4, 1000);
    check(d.kept == 4, "the first look after a pause took a period away on "
                       "its own evidence");
    now = play(&d, now + 8, 260, 1);
    check(d.kept == 3, "a whole steady window after the pause did not");

    /* More queued than kept, as just after a step: not a bite of billions. */
    depth_begin(&d, 4, 1, 1, now);
    d.kept = 3;
    for (unsigned t = 0; t < 300; t += 4) {
        depth_look(&d, 4, now, 250);
        now += 4;
    }
    check(d.kept == 2, "a queue holding more than it keeps was read as a "
                       "bite too large to step on");

    if (failed) {
        printf("FAIL: %d of %d checks on depth.h\n", failed, checked);
        return 1;
    }

    printf("PASS: %d checks on depth.h (full at the start, one a window with two to "
           "spare, not inside a window, a dry run raises it for good, never past "
           "the size, and a steady queue asked for comes down only while it is "
           "taken a period at a time)\n", checked);
    return 0;
}
