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

int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
    __attribute__((format(printf, 3, 0)));

#endif /* STDIO_H */
