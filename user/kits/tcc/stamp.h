/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_TCC_STAMP_H
#define KOSMOS_TCC_STAMP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Kosmos's header written into an image TinyCC has linked (`docs/tinycc.md`,
 * C1): "KOSMOS" and how many bytes from the base are code, as `user/user.ld`
 * computes them for GCC's - the code segment's end, rounded up to a page.
 * The image is held to the layout first: an ELF64 for this machine, its
 * first loadable segment the code, read and execute, at `base`, its first
 * sixteen bytes the empty slot `head.c` left. Answers NULL, or why not.
 */
const char *tcc_stamp(unsigned char *file, size_t bytes, uint64_t base);

#endif /* KOSMOS_TCC_STAMP_H */
