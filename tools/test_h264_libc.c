/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The three things FFmpeg's objects want from Kosmos's C library that the
 * Mac's does not have by the same name, for `tools/test_h264.c`.
 *
 * The decoder is compiled for this test exactly as for the guest - against
 * `runtime/include/`, with the configuration `tools/ffmpeg_vendor.py` wrote
 * for Kosmos - so what is tested is what runs there. Every function those
 * headers declare is standard C and the Mac's libc answers it; these are
 * the exceptions:
 *
 * - `__errno`, which is what Kosmos's `errno` expands to, and the Mac
 *   spells `__error`;
 * - `panic`, which Kosmos's `assert` calls;
 * - `stderr`, which Kosmos declares as a variable and the Mac as a macro
 *   over `__stderrp`. FFmpeg's default log callback is its only user, and
 *   the kit replaces that callback before FFmpeg says anything.
 *
 * Compiled with the Mac's own headers, and so it includes none of
 * Kosmos's: the two sets cannot both be in one file.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

int *__errno(void);
void panic(const char *msg) __attribute__((noreturn));

int *__errno(void)
{
    return &errno;
}

void panic(const char *msg)
{
    fprintf(stderr, "panic: %s\n", msg);
    abort();
}

/* Kosmos's `stderr`, by its link name; `stdio.h` above made the word a
 * macro, so the object is named through the assembler instead. */
void *kosmos_stderr __asm__("_stderr") = NULL;
