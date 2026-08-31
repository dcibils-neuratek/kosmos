#ifndef HAL_H
#define HAL_H

#include <stdbool.h>
#include <stdint.h>

/*
 * What the board provides. One implementation per board, under hal/<board>/.
 *
 * `arch/` is "which CPU are you" and is reimplemented per architecture.
 * `hal/` is "which peripheral do you have" and is one interface with several
 * implementations, because pushing out a character is pushing out a
 * character on any board.
 *
 * Deliberately only what has been needed. Each of these arrived with the
 * milestone that needed it - the interrupt controller and the timer at M1,
 * the display at M6 - and none of it was written ahead of a caller.
 *
 * It still takes its real shape at M2's second half, once there is a second
 * target to compare against. An interface written against a single target is
 * that target's shape wearing generic names, and everything below is written
 * knowing that.
 */

/* The minimum required to have output. Called before anything else. */
void hal_early_init(void);

/* One character out the serial port. Blocks until there is room. */
void hal_putchar(char c);

/* One character in, or HAL_NO_INPUT when none is waiting. Never blocks:
 * a REPL has to stay responsive to a timer tick while it waits, and from M6
 * input has to keep flowing while an application is busy drawing. */
#define HAL_NO_INPUT    (-1)

int hal_getchar(void);

/* Where usable RAM is. One contiguous range is enough for every target so
 * far; a board with holes in its map would need a list, and that is the
 * moment to change this, not before. */
struct memrange {
    unsigned long base;
    unsigned long size;
};

void hal_ram_range(struct memrange *out);

/* The interrupt controller. hal_irq_handle() is called from the IRQ vector:
 * it acknowledges, services and signals end-of-interrupt. */
void hal_irq_init(void);
void hal_irq_handle(void);

/* The tick source. hal_timer_init needs hal_irq_init first. */
void hal_timer_init(unsigned hz);
unsigned long hal_ticks(void);

/*
 * How many deadlines have been missed: the timer came due again before the
 * previous interrupt was serviced, so the tick for it never happened.
 *
 * It means the system fell behind, and it is the difference between a tick
 * count and a clock. Under load the two diverge, and anything measuring
 * elapsed time from ticks is measuring something else. The counter is the
 * clock; ticks are a heartbeat.
 */
unsigned long hal_ticks_missed(void);

/*
 * The display.
 *
 * A linear framebuffer and nothing else. What is above this line is the app
 * server's problem, and `gfx.md` is emphatic that it stays that way: the
 * kernel does not know what a window is, and the board does not know what a
 * pixel means beyond its format.
 *
 * `pitch` is bytes per row and is **not** width * 4. Under QEMU this board
 * chooses the stride itself and deliberately pads it, because a display that
 * hands back exactly width * 4 lets every address calculation in the system
 * be written wrong and still work - right up until the first real board,
 * where the pitch is whatever the firmware felt like. `gfx.md` §19.3 puts
 * this at the top of its list of traps.
 *
 * One format, XRGB8888: a uint32_t per pixel, 0x00RRGGBB. Not negotiable
 * from up here. When a board arrives that cannot do it, that is the moment
 * the interface grows a format field, and not before.
 */
struct fb {
    volatile uint32_t *pixels;
    unsigned width;
    unsigned height;
    unsigned pitch;             /* bytes per row; never assume width * 4 */
};

/*
 * Brings the display up and describes it. False when there is none, which
 * is not an error: a serial-only boot is a legitimate way to run, and the
 * system has to keep working without a screen.
 *
 * The board decides where the pixels live, because that is the one thing the
 * two targets disagree about. Under QEMU the guest points ramfb at memory it
 * chose; on the Pi the firmware answers the mailbox with an address it chose.
 * A caller that supplied the memory would be right on one board and wrong on
 * the other.
 */
bool hal_fb_init(struct fb *out);

/*
 * Brings up a keyboard, if the board has one. False is not an error: input
 * arriving over the serial line is how this system ran until M6 and is how
 * it runs on a board with a cable and no keyboard.
 *
 * There is deliberately no hal_keyboard_getchar. A keyboard is a source of
 * characters and `hal_getchar` is where characters come from, so the board
 * answers from whichever of its sources has one. Nothing above the HAL
 * changes because a keyboard exists.
 */
bool hal_keyboard_init(void);

#endif /* HAL_H */
