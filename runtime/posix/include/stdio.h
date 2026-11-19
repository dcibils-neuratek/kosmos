/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_STDIO_H
#define POSIX_STDIO_H

/*
 * **`stdio` for ported code** (`docs/w3.md` W3d3): the runtime's
 * declarations, with streams over descriptors behind them
 * (`runtime/posix/stdio.c`), and the calls POSIX adds.
 */
#include_next <stdio.h>

#include <stdarg.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#ifndef FILENAME_MAX
#define FILENAME_MAX 512
#endif

int     getchar(void);
int     putc(int c, FILE *f);
FILE   *fdopen(int fd, const char *mode);
FILE   *tmpfile(void);
int     setvbuf(FILE *f, char *buf, int mode, size_t size);
void    setbuf(FILE *f, char *buf);
ssize_t getline(char **line, size_t *room, FILE *f);
ssize_t getdelim(char **line, size_t *room, int delimiter, FILE *f);
int     fseeko(FILE *f, off_t offset, int whence);
off_t   ftello(FILE *f);
void    perror(const char *s);

/* The text printed into memory `malloc` gave, `*out` freed by the caller;
 * its length, or -1 with `*out` unchanged. */
int     asprintf(char **out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int     vasprintf(char **out, const char *fmt, va_list ap);

/* A stream written into memory that grows: `*text` and `*length` say what
 * was written, a nought after it, at every `fflush` and `fclose`; the
 * caller frees `*text` (`stdio.c`). */
FILE   *open_memstream(char **text, size_t *length);

__END_DECLS

#endif /* POSIX_STDIO_H */
