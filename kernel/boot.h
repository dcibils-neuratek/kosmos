#ifndef KERNEL_BOOT_H
#define KERNEL_BOOT_H

/*
 * The boot, narrated.
 *
 * This is a learning system, and what it does between the reset vector and
 * a prompt is most of what there is to learn. So it says: one numbered line
 * per stage on the serial port and on the screen, and a bar at the bottom
 * that fills as they complete.
 *
 * The count is a constant rather than something discovered, because a
 * progress bar that does not know its total is a spinner with extra steps.
 * Adding a stage means changing BOOT_STAGES, and there is a test that the
 * two agree - a bar that reaches 80% and stops is worse than no bar.
 */

/* How many stages there are between the reset vector and userland. */
#define BOOT_STAGES 10

/* Announces one, advances the bar. `what` is the short name; anything more
 * specific is printed after it by the caller, indented by boot_detail. */
void boot_stage(const char *what);

/* An indented line under the stage just announced: the numbers that make the
 * stage worth watching. */
void boot_detail(const char *text);

/* How many stages have been announced. For the test that BOOT_STAGES is
 * still the truth. */
unsigned boot_stages_done(void);

#endif /* KERNEL_BOOT_H */
