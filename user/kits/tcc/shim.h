/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_TCC_SHIM_H
#define KOSMOS_TCC_SHIM_H

/*
 * What TinyCC asks of a Unix, answered inside the C Kit and nowhere else
 * (`docs/tinycc.md`, step C3).
 *
 * **Not the libc's.** Kosmos's `unistd.h` is empty on purpose - no POSIX
 * personality at system level (`CLAUDE.md`) - and its stdio reads only
 * bytes a process has handed it. TinyCC opens its sources and headers by
 * name, reads them through descriptors, and writes its output through
 * `fdopen`. So this header is forced into TinyCC's one translation unit
 * (`-include`), its names put over TinyCC's calls by macro, and `shim.c`
 * answers them:
 *
 *   - a file opened for reading is asked of the kit's caller - a Lua
 *     function, which reads it through the namespace into a region - so
 *     only what TinyCC actually includes is read, and its bytes never pass
 *     through the interpreter;
 *   - a file opened for writing is memory the kit owns, handed back to Lua
 *     when the build is done, which writes it through the namespace.
 *
 * Descriptors here are this kit's, numbered from 3, for one build at a time.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef long ssize_t;

#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040
#define O_TRUNC    0x0200
#define O_BINARY   0

int     tcck_open(const char *path, int flags, ...);
ssize_t tcck_read(int fd, void *buf, size_t n);
long    tcck_lseek(int fd, long offset, int whence);
int     tcck_close(int fd);
FILE   *tcck_fdopen(int fd, const char *mode);
size_t  tcck_fwrite(const void *p, size_t size, size_t n, FILE *f);
int     tcck_fputc(int c, FILE *f);
int     tcck_fclose(FILE *f);
int     tcck_unlink(const char *path);
char   *tcck_getcwd(char *buf, size_t size);
char   *tcck_realpath(const char *path, char *resolved);
char   *tcck_getenv(const char *name);
int     tcck_gettimeofday(void *tv, void *tz);

#define open        tcck_open
#define read        tcck_read
#define lseek       tcck_lseek
#define close       tcck_close
#define fdopen      tcck_fdopen
#define fwrite      tcck_fwrite
#define fputc       tcck_fputc
#define fclose      tcck_fclose
#define unlink      tcck_unlink
#define getcwd      tcck_getcwd
#define realpath    tcck_realpath
#define getenv      tcck_getenv
#define gettimeofday tcck_gettimeofday

/* Long doubles are the processor's on AArch64 and the x87's on x86-64; the
 * libc has neither's arithmetic, so TinyCC parses their literals as
 * doubles - as it does on a compiler without them. */
long double tcck_strtold(const char *s, char **end);
long double tcck_ldexpl(long double x, int e);
#define strtold     tcck_strtold
#define ldexpl      tcck_ldexpl

#endif /* KOSMOS_TCC_SHIM_H */
