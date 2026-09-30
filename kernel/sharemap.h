/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KERNEL_SHAREMAP_H
#define KERNEL_SHAREMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What a process has mapped in its share window, and where.
 *
 * **A record for every mapping** (`roadmap.md` 6zz f, `testing.md` 18.297).
 * The window had none: a bump pointer said where the next mapping went and
 * the page tables were the only other trace. Two things followed from that,
 * and both were found the day the network stack began giving its rings back.
 *
 *   - **A mapping did not hold its region.** A region's pages live as long as
 *     a capability to it does, so a process that mapped one and dropped its
 *     last capability kept a mapping onto pages the kernel would hand to
 *     somebody else. Now a record holds a reference, taken when the mapping
 *     is made and let go when it is unmapped or the process ends: while any
 *     process maps a region, its pages stay.
 *   - **Addresses came back only newest-first.** Unmapping lowered the bump
 *     pointer when the mapping at the top went and never otherwise, so a
 *     server that let rings go out of order - which a network stack does -
 *     ran out of window however little it had mapped. Now a mapping goes in
 *     the lowest gap it fits, and a hole is a gap like any other.
 *
 * The records are kernel objects in a pool that grows, as every pool here
 * does, with a ceiling of one for every page of memory - more mappings than
 * that would be mappings of nothing. A process's own are a list in address
 * order, which is what finding a gap needs; one lock for every list, held
 * for a walk and a link and nothing else.
 */

struct memobj;
struct process;

struct sharemap {
    bool             in_use;
    bool             ready;         /* its pages are in: it may be unmapped */
    uintptr_t        va;
    size_t           pages;
    struct memobj   *region;        /* held while mapped; NULL for a device */
    struct sharemap *next;          /* the owner's next, by address */
};

void sharemap_init(void);

/*
 * Room for `pages` in the process's share window, in the lowest gap that
 * fits, recorded as holding `region` - whose reference the record now owns,
 * or NULL for a device's window. NULL when nothing fits, and the reference is
 * then still the caller's. The record is not `ready` until the caller has
 * mapped the pages and said so.
 */
struct sharemap *sharemap_place(struct process *p, size_t pages,
                                struct memobj *region);

/* The pages are in; it may be taken out. */
void sharemap_ready(struct sharemap *s);

/* A placing that could not be finished, taken back. The region's reference
 * is the caller's again. */
void sharemap_forget(struct process *p, struct sharemap *s);

/*
 * The mapping of exactly `pages` at `va`, out of the process's record, and
 * its region through `*region` for the caller to let go of once the page
 * tables no longer map it. False when there is no such mapping, or it is
 * still being made.
 */
bool sharemap_take(struct process *p, uintptr_t va, size_t pages,
                   struct memobj **region);

/* Every mapping a process has, let go of - after its address space is. */
void sharemap_release_all(struct process *p);

/* Records in use, across every process. */
unsigned sharemap_in_use(void);

#endif
