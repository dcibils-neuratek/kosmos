/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef STDIO_H
#define STDIO_H

#include <stdarg.h>
#include <stddef.h>

/*
 * Formatting, and reading files a process was handed.
 *
 * There is no global tree, so a bare path means nothing. Lua's file library
 * is not built (`fopen("/etc/passwd")` is semantically incoherent in Kosmos,
 * per `design.md` §5.4), and this is not a POSIX stdio: what `fopen` opens is
 * what the process named itself with `kosmos_provide`, which is how Doom's
 * WAD and Quake's pak arrive. `design.md` §17.3 draws the line - I/O that
 * resolves against the process's own namespace and nowhere else.
 */

#define EOF     (-1)

/*
 * Incomplete here, on purpose.
 *
 * `stdio.c` defines it and nothing else can see inside: a FILE is a position
 * in bytes a process provided. `lauxlib.h` declares a `luaL_Stream` holding a
 * `FILE *` unconditionally, and an incomplete type is enough for that.
 */
typedef struct _kosmos_file FILE;

/*
 * The stream functions, and what each does here.
 *
 * Reading works on a provided file - `fopen` for reading, `fread`, `getc`,
 * `fscanf`, `feof`. Opening anything else, and writing, fail: `fopen` sets
 * `ENOENT`, and Lua's `luaL_loadfile` says "cannot open", which is true. See
 * `stdio.c`.
 */
#define BUFSIZ  1024

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE  *fopen(const char *path, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *f);
int    fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t count, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);
int    fflush(FILE *f);
int    getc(FILE *f);
int    fgetc(FILE *f);
int    ungetc(int c, FILE *f);

/*
 * The printing half, which goes to this process's console.
 *
 * `FILE *` is accepted and ignored: both stream pointers are NULL here and
 * a process has one console, not two. See the note in stdio.c.
 */
int printf(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int vprintf(const char *fmt, va_list ap)
    __attribute__((format(printf, 1, 0)));
int vfprintf(FILE *f, const char *fmt, va_list ap)
    __attribute__((format(printf, 2, 0)));
int sprintf(char *buf, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int vsprintf(char *buf, const char *fmt, va_list ap)
    __attribute__((format(printf, 2, 0)));
int sscanf(const char *in, const char *fmt, ...)
    __attribute__((format(scanf, 2, 3)));
int vsscanf(const char *in, const char *fmt, va_list ap)
    __attribute__((format(scanf, 2, 0)));
int fscanf(FILE *f, const char *fmt, ...)
    __attribute__((format(scanf, 2, 3)));
int vfscanf(FILE *f, const char *fmt, va_list ap)
    __attribute__((format(scanf, 2, 0)));

/*
 * The scanner under all four, bounded by a length rather than a NUL and
 * reporting how much it consumed - both of which `fscanf` needs, because a
 * file here is bytes in memory. `runtime/libc/scan.c` has the account.
 */
int kosmos_vscan(const char *in, size_t len, const char *fmt, va_list ap,
                 size_t *used);

/*
 * Hand this libc a file the process already holds.
 *
 * `name` is matched by its last path component, and the bytes must outlive
 * every `fopen` of it. See the long note in stdio.c: this is not a global
 * tree, it is one process saying what a name means to it.
 */
int kosmos_provide(const char *name, const void *bytes, size_t len);

int puts(const char *s);

/* Positioning, which works on a provided file, and the two calls that would
 * change a tree, which refuse. See the notes in stdio.c. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int  fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int  remove(const char *path);
int  rename(const char *from, const char *to);
int putchar(int c);
int fputs(const char *s, FILE *f);
int fputc(int c, FILE *f);

int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
    __attribute__((format(printf, 3, 0)));

#endif /* STDIO_H */
