/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KERNEL_POOL_H
#define KERNEL_POOL_H

/*
 * A pool of kernel objects that grows - threads, processes, address spaces,
 * endpoints, regions - written once for all of them (`threads.md` step 1b).
 *
 * **Why a pool and not a heap**, which is `CLAUDE.md`'s *no heap for kernel
 * objects*: taking a slot is bounded time and cannot fragment; running out is
 * a clean refusal at the syscall rather than a failure halfway through an
 * operation with locks held; and no process can make the kernel consume
 * unbounded memory of its own.
 *
 * **Why it grows**, which that principle used to forbid: every pool was a
 * number compiled in for a 512 MB QEMU guest, and several bit - spawning
 * failed at twenty-six, a region stopped at 32 MB, a thread pool of
 * forty-eight could not have held threads in a process. Diego, 19 September
 * 2026: "We should be able to grow as needed on processes and threads just
 * like beos or Linux". Growing keeps every property above, provided it is
 * done this way:
 *
 *   - **In slabs**, a few pages of objects at a time, from `pmm`, so a claim
 *     is a scan or one slab - bounded.
 *   - **Never given back**, so nothing fragments and a pointer to an object
 *     is valid for as long as the kernel is: a slab never moves.
 *   - **To a ceiling**, from the machine's memory, which is what stops a
 *     process making the kernel consume without bound - as Linux computes
 *     `threads-max` from RAM at boot.
 *
 * **What it does not do is claim.** Each pool's owner knows what "free"
 * means for its objects - a thread slot is free when unused or dead and left,
 * an endpoint when its own lock says so - and claims under its own lock, as
 * it always did. The pool is where the objects live and how there come to be
 * more of them: an owner whose scan finds nothing calls `pool_grow` and scans
 * again.
 *
 * **The one word read without a lock is the slot count**, stored with release
 * after a slab's pointer and loaded with acquire before any object under it.
 * A reader on another core that sees the count sees the slab behind it; one
 * that reads a stale count does not look at the newest slab, every object of
 * which is still as `fresh` left it.
 */

#include <stdbool.h>
#include <stddef.h>

#include "spinlock.h"

struct pool {
    const char     *name;               /* for the boot screen and a panic */
    size_t          size;               /* bytes an object */
    unsigned        per_slab;           /* objects a slab */
    size_t          slab_pages;
    void          (*fresh)(void *object);   /* a new slab's objects, or NULL */

    void          **slab;               /* the directory: one pointer a slab */
    unsigned        slabs_max;          /* the ceiling, in slabs */
    unsigned        slots;              /* objects made: release / acquire */

    struct spinlock lock;               /* growth only */
};

/*
 * The directory and the first slabs: at least `boot` objects, at most
 * `ceiling`. `fresh`, if given, prepares each object of every new slab -
 * a zeroed page is already every object's "unused", unless it holds a lock,
 * whose holder has to say nobody. Panics if the machine cannot spare that
 * much at boot, because nothing can run without it.
 */
void pool_init(struct pool *p, const char *name, size_t size,
               unsigned ceiling, unsigned boot, void (*fresh)(void *object));

/* Objects made so far; everything below is valid to look at. */
unsigned pool_slots(const struct pool *p);

/* The most objects there can ever be. */
unsigned pool_ceiling(const struct pool *p);

/* Object `i`, which must be below `pool_slots`. */
void *pool_at(const struct pool *p, unsigned i);

/* One more slab. False at the ceiling, or when the pages are not there. */
bool pool_grow(struct pool *p);

/* How many objects a machine with this much memory affords at one for
 * every `each` bytes of it, and never fewer than `floor`. */
unsigned pool_ceiling_for(size_t each, unsigned floor);

#endif
