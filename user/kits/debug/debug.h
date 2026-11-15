/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Debug Kit's door** (`docs/debug.md`): a function's name from an
 * address, read from an image's `.symtab`.
 *
 * The kit never opens a file: the caller hands it a function that reads
 * `length` bytes at `offset` of the image, however that caller reads
 * files - the POSIX layer in JetSki, `stdio` on the Mac in the host test.
 * Opening reads the section headers and `.symtab`, and keeps the image's
 * functions sorted by address; a name is read from `.strtab` only when
 * it is asked for.
 *
 * Names come back as the image spells them - mangled, for C++. Whoever
 * links a demangler (`__cxa_demangle`, which every C++ image has) turns
 * them into words; the kit does not carry a second one.
 */

#ifndef KOSMOS_DEBUG_H
#define KOSMOS_DEBUG_H

#include <stddef.h>
#include <stdint.h>

/* Read `length` bytes at `offset` into `into`; the number read, short or
 * negative when the file has no more. */
typedef long (*debug_read_fn)(void *ctx, uint64_t offset, void *into,
                              size_t length);

struct debug_image;

/* NULL when the file is not a 64-bit ELF, has no symbol table, or memory
 * ran out; `*why`, when given, says which. */
struct debug_image *debug_open(debug_read_fn read, void *ctx,
                               const char **why);

/* The function holding `addr`: its name into `name` (always ended, cut to
 * fit), and how far into it `addr` is. 0 when found, -1 when no function
 * holds it. */
int debug_name(struct debug_image *image, uint64_t addr, char *name,
               size_t room, uint64_t *offset);

/* How many functions the image has - for the test, and for saying so. */
size_t debug_functions(const struct debug_image *image);

void debug_close(struct debug_image *image);

#endif
