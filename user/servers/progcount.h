/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_PROGCOUNT_H
#define KOSMOS_PROGCOUNT_H

#include <stdbool.h>
#include <stdint.h>

#include "deviceproto.h"

/*
 * **Which programs read and wrote a drive** (`.programs`, `deviceproto.h`;
 * Disk Monitor's D2): the one table the four servers behind a drive keep -
 * `diskfs`, `smbfs`, the drive server and `ramfs` - rather than four copies
 * of it. A row a process, grown as processes arrive and never a fixed
 * number of them; and held to `tools/test_progcount.c` on the Mac, since it
 * is C that every drive's requests pass through.
 */
struct progcount {
    struct disk_program *rows;
    unsigned count;
    unsigned room;
};

/* A read or a write of `bytes` by `process`, at `path`. A row that cannot be
 * made - the heap refused - is a request not counted, never one refused. */
void progcount_note(struct progcount *t, uint32_t process, bool wrote,
                    uint64_t bytes, const char *path);

/* Up to `most` rows from `from` into `out`, how many, and whether more
 * follow. */
unsigned progcount_page(const struct progcount *t, unsigned from,
                        struct disk_program *out, unsigned most, uint32_t *more);

#endif /* KOSMOS_PROGCOUNT_H */
