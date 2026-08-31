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
#define BOOT_STAGES 12

/*
 * A stage reads in three parts, and each is a different question:
 *
 *   boot_stage("physical memory")     what is happening
 *   boot_why("...")                   what it is for, and why it is here
 *   boot_fact("512 MB in 131072 ...") what it actually found
 *
 * The middle one is the reason this exists. A log that says "physical
 * memory" and a number teaches nothing to somebody who does not already
 * know why an operating system needs a page allocator before it can do
 * anything else. The point of this system is to be read.
 */

/* Announces a stage and advances the bar. */
void boot_stage(const char *what);

/* What the stage is for. Dim, indented, and as many lines as it takes. */
void boot_why(const char *text);

/* What it found: an indented "-> " line. */
void boot_fact(const char *text);

/* The same, when the fact has numbers in it and has to be built up with
 * kputu and kputs. Ends with a newline; do not print one. */
void boot_fact_begin(void);
void boot_fact_end(void);

/* How many stages have been announced. For the test that BOOT_STAGES is
 * still the truth. */
unsigned boot_stages_done(void);

#endif /* KERNEL_BOOT_H */
