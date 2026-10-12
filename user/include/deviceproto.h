/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DEVICEPROTO_H
#define KOSMOS_DEVICEPROTO_H

#include <stdint.h>

/*
 * **What a drive has cost the server that holds it** - `.device` under the
 * drive's path - in one shape for every drive (`docs/diskmonitor.md`, D1).
 *
 * `/Home`'s (`diskfs`) and a share's (`smbfs`) came first, for Disk
 * Benchmark, in `diskproto.h`; a stick's (`drives`) and memory's (`ramfs`)
 * speak protocols of their own, and answer in this same struct rather than
 * each in a copy of it - so Disk Monitor reads every drive one way, and a
 * field added here is added for all four.
 *
 * Running totals since the server started, never rates: a rate is two
 * readings' difference, and only the reader knows how far apart. The times
 * are the **counter's** ticks (`counter_hz` a second), and say so.
 *
 * A share's (smbfs): `reads` and `read_bytes` are SMB READs and what they
 * brought, `read_counter_ticks` the time callers waited on them;
 * `cache_hits` the questions about names answered from memory and
 * `cache_misses` the folders asked of the server; and the last four are
 * the network's - every SMB request sent, the folder listings asked for,
 * the requests those took and the time they took. Zero where a drive has
 * no such thing: a stick is never written here, and memory has no cache.
 */
struct disk_device {
    uint64_t reads, writes;
    uint64_t read_bytes, write_bytes;
    uint64_t read_counter_ticks, write_counter_ticks;
    uint64_t cache_hits, cache_misses;
    uint64_t requests;          /* a share's: SMB requests sent */
    uint64_t listings;          /* a share's: folder listings asked for */
    uint64_t listing_requests;  /* the SMB requests those took */
    uint64_t listing_counter_ticks;
};

_Static_assert(sizeof(struct disk_device) == 96, "twelve counters");

/*
 * **Which programs, `.programs`** (Disk Monitor's D2): a row for each
 * process that has read or written this drive, its totals and the last path
 * it touched, eight rows a page - a request's `offset` the first row
 * wanted, the reply's `count` the rows in it and `more` whether others
 * follow. Rows in the order the server first met each program; the reader
 * sorts them. `process` is the id `SYS_SENDER` named, which is
 * `sys.processes()`'s; a process that has ended keeps its row, its name
 * gone from that list.
 */
#define DEVICE_LAST_MAX       64u
#define DEVICE_PROGRAMS_PAGE   8u

struct disk_program {
    uint32_t process;
    uint32_t reserved;
    uint64_t reads, writes;
    uint64_t read_bytes, write_bytes;
    char     last[DEVICE_LAST_MAX];     /* the path, its end kept if cut */
};

_Static_assert(sizeof(struct disk_program) == 104, "a program's row is 104 bytes");
_Static_assert(DEVICE_PROGRAMS_PAGE * sizeof(struct disk_program) <= 1024,
               "a page of programs fits where a page of bytes does");

#endif /* KOSMOS_DEVICEPROTO_H */
