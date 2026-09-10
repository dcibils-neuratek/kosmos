/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H

#include <stddef.h>

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

/*
 * The same screen, at a different address.
 *
 * **Not a second attach, and the difference is the whole point.** A board
 * that answered `hal_fb_early` gave an address the boot page tables
 * describe; `mmu_init` then replaces those tables and the same pixels
 * answer somewhere else. What is already drawn is *in the memory both
 * addresses name*, so this changes where the console writes and repaints
 * nothing - where attaching again would clear the screen and replay a
 * buffer that has already been consumed.
 *
 * The geometry must not have changed. It is the same framebuffer; if a
 * board ever hands back a different size here, that is a bug in the board
 * rather than something this should try to absorb.
 */
void console_rebase_screen(const struct fb *fb);

/*
 * Stop writing to the screen; the serial line keeps everything.
 *
 * One caller: a board whose early framebuffer could not be remapped after
 * `mmu_init`. Without this the console would go on writing into an address
 * that no longer translates, which is a fault inside a console write and
 * therefore a deadlock rather than a message.
 */
void console_detach_screen(void);

/* The colour of subsequent text on the screen, as 0xAARRGGBB. The serial
 * side has no opinion and ignores it. */
void console_colour(unsigned long foreground);

/*
 * A run of text in one colour, written as one operation.
 *
 * **The colour travels with the bytes rather than being a mode somebody
 * sets**, and that is the whole reason this exists next to `console_colour`.
 * The kernel can set a colour, print and set it back because the kernel is
 * the only writer; the console server is not - it serves every program at
 * once, and a Terminal serves its children while they interleave. Two
 * writers doing set-print-restore race over one `fg` and each other's colour
 * lands in the wrong line. A colour attached to the bytes cannot.
 *
 * `colour` of zero means "whatever the console is already using", so a
 * caller with no opinion says nothing and nothing changes for it.
 *
 * One lock for the whole run, which `kputc` in a loop was not: `sys_write`
 * took and released the console lock once per byte, so a four-kilobyte
 * write was four thousand acquisitions and two cores could interleave
 * halfway through a word.
 */
void kwrite_colour(const char *s, unsigned long len, unsigned long colour);

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

/*
 * The most recent bytes this console printed, into a caller's buffer.
 *
 * Everything goes through `kputc`, including every process's output - a
 * process prints by asking the console server and the console server calls
 * `sys.write` - so this is one place with all of it, in order.
 */
/*
 * How much that ring holds, and therefore the most `console_log` can ever
 * return. Named here rather than in `console.c` because the syscall that
 * exposes it has to cap what a caller asks for, and a second copy of this
 * number living in `syscall.c` is the kind of pair that drifts.
 */
#define CONSOLE_LOG_BYTES 65536

size_t console_log(char *out, size_t max);

void console_tick(void);

#endif /* KERNEL_CONSOLE_H */
