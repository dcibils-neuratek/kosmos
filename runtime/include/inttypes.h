/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef INTTYPES_H
#define INTTYPES_H

/*
 * The format macros, and why this file exists at all.
 *
 * `inttypes.h` is not one of the headers a freestanding C implementation
 * has to provide - the standard lists stdint, stddef, stdbool, stdarg,
 * limits, float, iso646, stdalign and stdnoreturn, and stops. So whether
 * `#include <inttypes.h>` works depends on what the *toolchain vendor*
 * chose to bundle beside the compiler.
 *
 * **It works on one of this project's two toolchains and not the other**,
 * and that is how it was found: ARM's official GNU toolchain ships newlib's
 * headers, Homebrew's `x86_64-elf-gcc` ships the freestanding set, and
 * libdom's public header includes this one. The ARM build had been quietly
 * compiling against a libc this system does not use and does not want -
 * which is the exact dependency `runtime/include/` exists to remove, and it
 * had a hole in it that nobody could see while there was one compiler.
 *
 * Three macros, because three are used: `PRIu32`, `PRIuMAX` and `PRIuPTR`,
 * across libdom and libcss. The rest of C99's set is not here for the
 * reason nothing else in this directory is complete either - a libc grown
 * to fit what a port asked for is a libc somebody can read.
 *
 * `l` rather than `ll` for the 64-bit ones: both targets are LP64, where a
 * long is sixty-four bits and `%lu` is what a `uintmax_t` and a `uintptr_t`
 * print with. A 32-bit target would need this file to say something else,
 * and Kosmos has decided it will not have one.
 */

#include <stdint.h>

#define PRId32      "d"
#define PRIi32      "i"
#define PRIu32      "u"
#define PRIx32      "x"
#define PRIX32      "X"

#define PRId64      "ld"
#define PRIi64      "li"
#define PRIu64      "lu"
#define PRIx64      "lx"
#define PRIX64      "lX"

#define PRIdMAX     "ld"
#define PRIuMAX     "lu"
#define PRIxMAX     "lx"

#define PRIdPTR     "ld"
#define PRIuPTR     "lu"
#define PRIxPTR     "lx"

#endif /* INTTYPES_H */
