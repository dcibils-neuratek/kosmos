#ifndef HAL_H
#define HAL_H

/*
 * What the board provides. One implementation per board, under hal/<board>/.
 *
 * `arch/` is "which CPU are you" and is reimplemented per architecture.
 * `hal/` is "which peripheral do you have" and is one interface with several
 * implementations, because pushing out a character is pushing out a
 * character on any board.
 *
 * Deliberately only what M0 needs. The framebuffer, the interrupt
 * controller, the timer and the tick counter arrive with the milestone that
 * needs them, and the interface takes its real shape at M2, once there is a
 * second target to compare against. An interface written against a single
 * target is that target's shape wearing generic names.
 */

/* The minimum required to have output. Called before anything else. */
void hal_early_init(void);

/* One character out the serial port. Blocks until there is room. */
void hal_putchar(char c);

#endif /* HAL_H */
