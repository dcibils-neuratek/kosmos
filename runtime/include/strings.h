/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef STRINGS_H
#define STRINGS_H

#include <stddef.h>

/*
 * The case-insensitive half of the string functions, which POSIX keeps in
 * its own header for a reason that stopped mattering in 1989.
 *
 * `strcasecmp` and `strncasecmp` are not ISO C - `<string.h>` has never
 * declared them - so a portable program includes this instead, and Doom is
 * one: `doomtype.h` includes it and calls `strncasecmp` a hundred and forty
 * times.
 *
 * **This is the third header found missing the same way**, after
 * `<inttypes.h>` and the maths that came from newlib's `libm.a`. Every one
 * of them worked on ARM because ARM's official GNU toolchain ships newlib
 * beside the compiler, and failed on x86-64 because Homebrew's
 * `x86_64-elf-gcc` ships the freestanding set and nothing else. The
 * pattern is worth naming: **a build that compiles is not evidence that a
 * dependency was decided on**, and a second toolchain is what asks.
 *
 * The functions themselves have been in `runtime/libc/string.c` all along,
 * next to the ones `<string.h>` declares. Only the declaration was missing.
 */

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);


#endif /* STRINGS_H */
