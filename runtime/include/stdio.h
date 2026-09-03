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

/*
 * Declared and never defined, on purpose.
 *
 * There are no streams here. This exists because lauxlib.h declares a
 * `luaL_Stream` holding a `FILE *`, unconditionally, even though only
 * liolib ever touches it and liolib is not built. An incomplete type is
 * enough for a pointer, and it means any attempt to actually use a FILE is
 * a compile error rather than a runtime surprise.
 */
typedef struct _kosmos_file FILE;

/*
 * The stream functions exist, and every one of them fails.
 *
 * That is not a placeholder, it is the answer. Lua's `luaL_loadfile` opens a
 * path, and there is no path to open: what a process can reach is what was
 * mounted into its namespace, and a bare filename means nothing outside one.
 * Making them fail means `loadfile` returns "cannot open", which is true,
 * instead of the function not existing and the build not linking.
 *
 * Real file access arrives at M5 as `fs`, speaking the namespace protocol.
 * A real stdio arrives at M10 for Doom, and every one of its I/O functions
 * will resolve against the process's namespace and nowhere else. That is the
 * line `design.md` §17.3 draws.
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
int sscanf(const char *in, const char *fmt, ...)
    __attribute__((format(scanf, 2, 3)));
int vsscanf(const char *in, const char *fmt, va_list ap)
    __attribute__((format(scanf, 2, 0)));

/*
 * Hand this libc a file the process already holds.
 *
 * `name` is matched by its last path component, and the bytes must outlive
 * every `fopen` of it. See the long note in stdio.c: this is not a global
 * tree, it is one process saying what a name means to it.
 */
int kosmos_provide(const char *name, const void *bytes, size_t len);

int puts(const char *s);

/* The savegame half, all of which fail. See the note in stdio.c. */
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
