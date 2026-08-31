#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H

/*
 * The kernel's output. Sits directly on hal_putchar, because until M1 there
 * is no interrupt controller and no buffering: output has to keep working
 * when everything else in the system is already dead.
 *
 * From M6 it also goes to the screen when there is one, which is why
 * `console_attach_screen` exists. See console.c for why forty lines of glyph
 * blitting in the kernel is not the graphics subsystem CLAUDE.md forbids:
 * `panic()` writes through here, and on a board with no serial cable a panic
 * that prints into the void is the same as a machine that does not work.
 */

struct fb;

/* One character. A '\n' becomes "\r\n", because a serial terminal needs
 * both. Everything else goes out as it is, including zero bytes: Lua strings
 * may contain them and truncating there would silently lose data. */
void kputc(char c);

/* Writes the string as given. Does not append a newline. A '\n' is expanded
 * to "\r\n", because a serial terminal needs both. */
void kputs(const char *s);

/* Unsigned decimal. There is no printf and there will not be one until the
 * libc arrives at M2. */
void kputu(unsigned long v);

/* Lower-case hex, zero-padded to exactly `digits` digits, no "0x" prefix.
 * Fixed width on purpose: register dumps are read by scanning down a column,
 * and a variable-width value ruins that. */
void kputx(unsigned long v, unsigned digits);

/*
 * Sends everything printed from here on to the screen as well. Clears it,
 * puts the cursor at the top, and writes `title` there - on the screen only,
 * because the serial line has already had one. Called once, after the board
 * reports a display.
 */
void console_attach_screen(const struct fb *fb, const char *title);

/* The colour of subsequent text on the screen, as 0xAARRGGBB. The serial
 * side has no opinion and ignores it. */
void console_colour(unsigned long foreground);

/* The boot progress bar, in the rows at the bottom that text never scrolls
 * through. A no-op with no screen. */
void console_progress(unsigned done, unsigned total);

/*
 * Blinks the cursor. Called from the timer tick, because it is the one thing
 * on the screen that has to change without anybody printing.
 *
 * A no-op with no screen, and cheap enough for the interrupt path: it counts
 * to twenty-five and, twice a second, fills one 8x16 cell.
 */
/*
 * Whether this console may draw on the screen.
 *
 * A compositor takes it; the console falls back to the serial line, which
 * has always had everything anyway. Resuming clears and starts from the top,
 * because there is no scrollback to restore.
 */
void console_screen_suspend(void);
void console_screen_resume(void);

void console_tick(void);

#endif /* KERNEL_CONSOLE_H */
