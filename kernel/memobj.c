/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Memory two processes can both see. See memobj.h for why it is a
 * capability rather than a handle.
 */

#include <stddef.h>
#include <string.h>

#include "memobj.h"
#include "spinlock.h"
#include "pmm.h"
#include "page.h"
#include "panic.h"

static struct memobj objects[MEMOBJ_MAX];

void memobj_init(void)
{
    unsigned i;

    for (i = 0; i < MEMOBJ_MAX; i++) {
        objects[i].in_use = false;
        objects[i].generation = 1;
        objects[i].refs = 0;
        objects[i].pages = 0;
        objects[i].indexes = 0;
        objects[i].contiguous = false;
    }
}

void *memobj_page(const struct memobj *m, size_t i)
{
    if (m == NULL || i >= m->pages) {
        return NULL;
    }

    return m->index[i / MEMOBJ_PER_INDEX][i % MEMOBJ_PER_INDEX];
}

/*
 * Everything a half-built region has taken, given back, and the slot with
 * it. Called only from `memobj_create`, which claims the slot before it
 * starts - so releasing it is this function's job and not the caller's.
 */
static void unwind(struct memobj *m, size_t built)
{
    size_t i;

    for (i = 0; i < built; i++) {
        pmm_free_page(memobj_page(m, i));
    }

    for (i = 0; i < m->indexes; i++) {
        pmm_free_page(m->index[i]);
        m->index[i] = NULL;
    }

    m->indexes = 0;
    m->pages = 0;
    m->contiguous = false;
    m->in_use = false;
}

/*
 * The region pool's lock.
 *
 * This file already understood the shape of the problem before there were
 * two cores: the comment below claims the slot before building in it,
 * because a syscall runs with interrupts on and can be *preempted* between
 * finding a slot and finishing with it. That is the same window a second
 * core opens, one step wider - preemption interleaves two callers on one
 * core, and a second core runs them at the same instant.
 *
 * So the lock covers exactly what the comment already identified: the scan
 * and the claim. Everything after - hundreds of page allocations - happens
 * outside it, on a slot nobody else can now find.
 */
static struct spinlock objects_lock = SPINLOCK("regions");

struct memobj *memobj_create(size_t pages, bool contiguous)
{
    unsigned i;

    if (pages == 0 || pages > MEMOBJ_PAGES_MAX) {
        return NULL;
    }

    for (i = 0; i < MEMOBJ_MAX; i++) {
        struct memobj *m = &objects[i];
        size_t indexes = (pages + MEMOBJ_PER_INDEX - 1) / MEMOBJ_PER_INDEX;
        size_t k, n;
        unsigned long flags = spin_lock(&objects_lock);

        if (m->in_use) {
            spin_unlock(&objects_lock, flags);
            continue;
        }

        /*
         * The slot is claimed before anything is built in it.
         *
         * `memobj_create` runs in a syscall with interrupts on, so it can be
         * preempted between finding a free slot and finishing with it. While
         * `in_use` was still false, a second caller scanning for a free slot
         * would find the *same* one and both would build into it. That was
         * survivable when a region was one `pmm_alloc_contiguous` and a base
         * pointer; it is not now, when building one is a loop over hundreds
         * of pages writing into a shared index.
         *
         * Claiming first makes the window empty. Every failure below has to
         * release it again, which is what `unwind` does.
         */
        m->in_use = true;
        spin_unlock(&objects_lock, flags);

        m->pages = pages;
        m->indexes = 0;
        m->contiguous = false;

        for (k = 0; k < indexes; k++) {
            m->index[k] = pmm_alloc_page();

            if (m->index[k] == NULL) {
                unwind(m, 0);
                return NULL;
            }

            m->indexes = k + 1;
        }

        /*
         * **Unless the caller asked for one run, which is what a device
         * needs and nothing else does.**
         *
         * A USB controller walks its command ring itself, in physical
         * addresses, with no page table in the way - so those pages have to
         * be consecutive in physical memory or the hardware reads somebody
         * else's. That is the one requirement the scattered arrangement
         * above cannot meet, and it is why this flag exists rather than a
         * second kind of object: a DMA buffer is the same thing - pages a
         * process maps - with one constraint on where they come from.
         *
         * The cost is exactly the failure the scattered version was
         * introduced to remove: a run can be unavailable on a fragmented
         * machine while the pages exist. That is acceptable *here* and was
         * not acceptable there, and the difference is size - a ring is a
         * handful of pages where a window's double buffer was nine hundred.
         *
         * The pages still go in the index one by one, so `memobj_page` and
         * everything that maps or frees a region is untouched. The run is
         * remembered only so its physical address can be reported.
         */
        if (contiguous) {
            char *run = pmm_alloc_contiguous(pages);

            if (run == NULL) {
                unwind(m, 0);
                return NULL;
            }

            memset(run, 0, pages * PAGE_SIZE);

            for (n = 0; n < pages; n++) {
                m->index[n / MEMOBJ_PER_INDEX][n % MEMOBJ_PER_INDEX] =
                    run + n * PAGE_SIZE;
            }

            m->contiguous = true;
            m->refs = 1;

            return m;
        }

        /*
         * Otherwise the pages themselves, one at a time and in no particular
         * place. This is the change: a region no longer needs a run, so a
         * fragmented machine can still satisfy a large one.
         *
         * Zeroed before anybody can see it. A fresh region holding whatever
         * the last owner left is how one process reads another's memory
         * without either of them doing anything wrong - and this region is
         * about to be handed to a second process on purpose, which makes it
         * the last place to be casual about it.
         */
        for (n = 0; n < pages; n++) {
            void *page = pmm_alloc_page();

            if (page == NULL) {
                unwind(m, n);
                return NULL;
            }

            memset(page, 0, PAGE_SIZE);
            m->index[n / MEMOBJ_PER_INDEX][n % MEMOBJ_PER_INDEX] = page;
        }

        m->refs = 1;

        return m;
    }

    return NULL;
}

void memobj_ref(struct memobj *m)
{
    if (m != NULL && m->in_use) {
        m->refs++;
    }
}

void memobj_unref(struct memobj *m)
{
    if (m == NULL || !m->in_use) {
        return;
    }

    if (m->refs == 0) {
        panic("memobj: unreferenced twice");
    }

    m->refs--;

    if (m->refs > 0) {
        return;
    }

    /*
     * The last capability to it is gone, so the pages go back.
     *
     * The generation moves first. A stale capability in some thread's table
     * still points here, and the next region to take this slot must not be
     * reachable through it - which is the same protection an endpoint has
     * and for the same reason.
     */
    m->generation++;

    /*
     * The pages go back first and the *slot* last.
     *
     * `in_use = false` is what makes this descriptor claimable, and a
     * claimant immediately writes `pages` and the index pointers into it.
     * Releasing the slot before walking the index to free the pages means
     * walking an index the new owner is rewriting: pages freed twice, and
     * pages belonging to the new region freed out from under it.
     *
     * The generation still moves first, because that is what makes a stale
     * capability refuse before any of this happens.
     */
    {
        size_t i;

        for (i = 0; i < m->pages; i++) {
            pmm_free_page(memobj_page(m, i));
        }
    }

    {
        size_t k;

        for (k = 0; k < m->indexes; k++) {
            pmm_free_page(m->index[k]);
            m->index[k] = NULL;
        }

        m->indexes = 0;
    }

    m->pages = 0;
    m->in_use = false;
}

unsigned memobj_in_use(void)
{
    unsigned i;
    unsigned n = 0;

    for (i = 0; i < MEMOBJ_MAX; i++) {
        if (objects[i].in_use) {
            n++;
        }
    }

    return n;
}

unsigned memobj_total(void)
{
    return MEMOBJ_MAX;
}

uintptr_t memobj_phys(const struct memobj *m)
{
    /*
     * Refused for a scattered region rather than answering with its first
     * page, and that is the whole point of recording `contiguous` instead
     * of inferring it. A driver handed the first page of a region whose
     * pages are elsewhere would program its hardware to walk off the end of
     * it into whatever the allocator gave somebody else - and the hardware
     * does not consult a page table on the way.
     */
    if (m == NULL || !m->in_use || !m->contiguous || m->pages == 0) {
        return 0;
    }

    return (uintptr_t)memobj_page(m, 0);
}
