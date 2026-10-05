/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_RAMSTORE_H
#define KOSMOS_RAMSTORE_H

#include <stddef.h>
#include <stdint.h>

#include "ramproto.h"

/*
 * /Temporary's store: everything the server does except receive and
 * reply, so the host can hold it to its behaviour without a machine
 * (`tools/test_ramstore.c`), as `kfs.c` is held.
 *
 * `ceiling` is the most it may hold, in bytes - entries, their values and
 * the watches parked on it together. `ramfs.c` gives it half of the
 * machine's memory; a test gives it what it wants to fill. Called again,
 * it empties the store first.
 */
void ram_store_init(size_t ceiling);

/*
 * One request, as it arrived: answered through `ram_reply` now, or - a
 * watch whose answer has not changed - later, when a write changes it.
 */
void ram_answer(const void *data, size_t length, uint64_t sender);

/*
 * How the store answers somebody. The server's sends the reply with
 * `kosmos_reply`; a test's keeps what it was told.
 */
void ram_reply(uint64_t to, const struct ram_reply *rep);

/* What it holds, in bytes counted against the ceiling; how many entries
 * and parked watches. For a test, and for whoever asks one day. */
size_t   ram_store_held(void);
unsigned ram_store_entries(void);
unsigned ram_store_watches(void);

#endif /* KOSMOS_RAMSTORE_H */
