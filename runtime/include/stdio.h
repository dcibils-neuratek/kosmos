#ifndef STDIO_H
#define STDIO_H

#include <stdarg.h>
#include <stddef.h>

/*
 * Formatting, and nothing else.
 *
 * There is no FILE and there are no streams. Lua's file library is not built
 * (`fopen("/etc/passwd")` is semantically incoherent in Kosmos, per
 * `design.md` §5.4), and the pieces of Lua that are built only ever want
 * snprintf for turning numbers into text.
 *
 * When a real stdio arrives it will be at M10, for Doom, and every one of
 * its I/O functions will resolve against the process's namespace and nowhere
 * else. That is the line `design.md` §17.3 draws and it is why there is no
 * half of one here now.
 */

#define EOF     (-1)

int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
    __attribute__((format(printf, 3, 0)));

#endif /* STDIO_H */
