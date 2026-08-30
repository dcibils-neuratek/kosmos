#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H

/*
 * The kernel's only output. Sits directly on hal_putchar, because until M1
 * there is no interrupt controller and no buffering: output has to keep
 * working when everything else in the system is already dead.
 */

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

#endif /* KERNEL_CONSOLE_H */
