/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERVERS_DISKASK_H
#define KOSMOS_SERVERS_DISKASK_H

/*
 * **A disk server, asked from C** - `diskproto.h` from the client's side,
 * once. `smbd`, the keyring, the map's tiles and a sandbox's monitor each
 * had a `disk_ask` of their own, four copies of one request built and one
 * reply read, which is the defect `CLAUDE.md`'s premise names; a sandbox's
 * disk (`docs/linux.md` L1) would have been the fifth.
 *
 * A `struct diskask` is one door: the endpoint, and the request and reply
 * it is asked with - a few kilobytes each, so a caller keeps one rather
 * than a stack frame holding them.
 */

#include <stdbool.h>
#include <stdint.h>

#include "kosmos.h"
#include "diskproto.h"

struct diskask {
    long           ep;                  /* -1: no disk */
    struct message msg, rep;
};

/*
 * `op` on `path`, `bytes` from `offset`, with `region` (a capability, or
 * -1) travelling with it, and `length` bytes of `u` - data, or a second
 * path - copied in when `u` is not NULL. DISK_OK or the server's error;
 * DISK_ERR_NO_DISK when there is no door or the call failed.
 */
uint32_t diskask(struct diskask *d, uint32_t op, const char *path, uint32_t flags,
                 uint64_t offset, uint64_t bytes, long region, const void *u,
                 uint32_t length);

/* The last answer's reply. */
const struct disk_reply *diskask_said(const struct diskask *d);

/*
 * A region of at least `bytes`, made and mapped: `*cap` and `*at`. Kept: a
 * second call with a region already there answers true and changes
 * nothing; `diskask_drop` gives it back.
 */
bool diskask_region(long *cap, uint8_t **at, uint64_t bytes);
void diskask_drop(long *cap);

#endif
