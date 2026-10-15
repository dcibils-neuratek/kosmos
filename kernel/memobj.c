/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Memory two processes can both see. See memobj.h for why it is a
 * capability rather than a handle.
 */

#include <stddef.h>
#include <string.h>

#include "memobj.h"
#include "mmu.h"
#include "spinlock.h"
#include "pmm.h"
#include "page.h"
#include "panic.h"
#include "pool.h"

/*
 * **The pool, which grows** (`kernel/pool.h`, `threads.md` step 1b):
 * two hundred and fifty-six at boot, as there were, and a region for every
 * `MEMOBJ_RAM_EACH` of memory at most - a descriptor is 168 bytes, and a
 * region is at least a page, so the pages run out first.
 */
#define MEMOBJ_BOOT_SLOTS 256u
#define MEMOBJ_RAM_EACH   (16u * 1024u)

static struct pool objects;

/*
 * The largest region this machine will make: a gigabyte, which is what a
 * directory page of index pages can hold, or half of memory when that is
 * less. `memobj.h` is the argument.
 */
size_t memobj_pages_max(void)
{
    size_t addressable = MEMOBJ_PER_INDEX * MEMOBJ_PER_INDEX * MEMOBJ_PER_INDEX;
    size_t half = pmm_total_pages() / 2;

    return half < addressable ? half : addressable;
}

static struct memobj *object_at(unsigned i)
{
    return pool_at(&objects, i);
}

static void release(struct memobj *m);

/* A new slab's regions: zeroed, which is unused and empty, and generation
 * one - the first a capability can name. */
static void memobj_fresh(void *object)
{
    ((struct memobj *)object)->generation = 1;
}

void memobj_init(void)
{
    pool_init(&objects, "regions", sizeof(struct memobj),
              pool_ceiling_for(MEMOBJ_RAM_EACH, MEMOBJ_BOOT_SLOTS),
              MEMOBJ_BOOT_SLOTS, memobj_fresh);
}

/* Where index page `k` of a region is kept: its directory's slot for it. */
static void ***index_slot(const struct memobj *m, size_t k)
{
    return &m->top[k / MEMOBJ_PER_INDEX][k % MEMOBJ_PER_INDEX];
}

/* Where page `i` of a region is kept: its index page's slot for it. */
static void **page_slot(const struct memobj *m, size_t i)
{
    return &(*index_slot(m, i / MEMOBJ_PER_INDEX))[i % MEMOBJ_PER_INDEX];
}

void *memobj_page(const struct memobj *m, size_t i)
{
    if (m == NULL || i >= m->pages) {
        return NULL;
    }

    return *page_slot(m, i);
}

/*
 * **The lock a first touch takes**, so two touches of one page - the
 * monitor's and the guest's, on two cores - give it one page and not two.
 * One for every region: a fill is an allocation and a page of zeroes, and
 * two at once on different regions is not a case worth a lock each.
 */
static struct spinlock fill_lock = SPINLOCK("region fill");

void *memobj_fill(struct memobj *m, size_t i)
{
    void **slot;
    void *page;
    unsigned long flags;

    if (m == NULL || i >= m->pages) {
        return NULL;
    }

    slot = page_slot(m, i);

    if ((page = __atomic_load_n(slot, __ATOMIC_ACQUIRE)) != NULL) {
        return page;
    }

    flags = spin_lock(&fill_lock);

    if ((page = *slot) == NULL && (page = pmm_alloc_page()) != NULL) {
        /* Zeroed before anybody can see it, as every region is. */
        memset(page, 0, PAGE_SIZE);
        __atomic_store_n(slot, page, __ATOMIC_RELEASE);
        m->touched++;
    }

    spin_unlock(&fill_lock, flags);
    return page;
}

size_t memobj_touched(const struct memobj *m)
{
    return m == NULL ? 0 : m->on_touch ? m->touched : m->pages;
}

/* Its index given back: the index pages, the directory pages, the top. */
static void free_index(struct memobj *m)
{
    size_t k;

    for (k = 0; k < m->indexes; k++) {
        pmm_free_page(*index_slot(m, k));
        *index_slot(m, k) = NULL;
    }

    for (k = 0; k < m->dirs; k++) {
        pmm_free_page(m->top[k]);
        m->top[k] = NULL;
    }

    if (m->top != NULL) {
        pmm_free_page(m->top);
        m->top = NULL;
    }

    m->indexes = 0;
    m->dirs = 0;
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
        if (memobj_page(m, i) != NULL) {
            pmm_free_page(memobj_page(m, i));
        }
    }

    free_index(m);
    m->pages = 0;
    m->contiguous = false;
    m->on_touch = false;
    m->touched = 0;
    release(m);
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
 * outside it, on a slot nobody else can now find. **And the reference count**,
 * since 19 September: see `memobj_ref`.
 */
static struct spinlock objects_lock = SPINLOCK("regions");

/* A slot given back, under the lock that claims them (`memobj_unref`). */
static void release(struct memobj *m)
{
    unsigned long flags = spin_lock(&objects_lock);

    m->in_use = false;
    spin_unlock(&objects_lock, flags);
}

struct memobj *memobj_create(size_t pages, bool contiguous, bool on_touch)
{
    unsigned i;

    if (pages == 0 || pages > memobj_pages_max()) {
        return NULL;
    }

    /* The pages themselves and the index pages above them, against the
     * reserve the kernel keeps for its own allocations (`pmm.c`). */
    if (contiguous && on_touch) {
        return NULL;                    /* a run is had at once or not at all */
    }

    if (!pmm_room_for_user((on_touch ? 0 : pages) + pages / MEMOBJ_PER_INDEX
                           + pages / MEMOBJ_PER_INDEX / MEMOBJ_PER_INDEX + 3)) {
        return NULL;
    }

again:
    for (i = 0; i < pool_slots(&objects); i++) {
        struct memobj *m = object_at(i);
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
        m->on_touch = false;
        m->touched = 0;
        m->dirs = 0;
        m->top = pmm_alloc_page();      /* the page of directory pages */

        if (m->top == NULL) {
            unwind(m, 0);
            return NULL;
        }

        memset(m->top, 0, PAGE_SIZE);

        for (k = 0; k < indexes; k++) {
            /* A directory page each 512 index pages, as they are reached. */
            if (k % MEMOBJ_PER_INDEX == 0) {
                m->top[k / MEMOBJ_PER_INDEX] = pmm_alloc_page();

                if (m->top[k / MEMOBJ_PER_INDEX] == NULL) {
                    unwind(m, 0);
                    return NULL;
                }

                memset(m->top[k / MEMOBJ_PER_INDEX], 0, PAGE_SIZE);
                m->dirs = k / MEMOBJ_PER_INDEX + 1;
            }

            *index_slot(m, k) = pmm_alloc_page();

            if (*index_slot(m, k) == NULL) {
                unwind(m, 0);
                return NULL;
            }

            m->indexes = k + 1;

            /* Empty slots read NULL, which a region given on touch relies
             * on and every other kind fills straight over. */
            memset(*index_slot(m, k), 0, PAGE_SIZE);
        }

        /* **Given on touch**: the index is all there is until somebody
         * touches a page (`memobj_fill`). */
        if (on_touch) {
            m->on_touch = true;
            m->refs = 1;

            return m;
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
                *page_slot(m, n) = run + n * PAGE_SIZE;
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
            *page_slot(m, n) = page;
        }

        m->refs = 1;

        return m;
    }

    /* Every region taken: one more slab and look again, or refuse. */
    if (pool_grow(&objects)) {
        goto again;
    }

    return NULL;
}

/*
 * **The count is changed under the pool's lock**, and it was not until 19
 * September (`threads.md` step 0).
 *
 * It was a plain `++` and `--`, and those are three steps each - read, add,
 * write - reached from different processes on different cores: a capability
 * delivered in a message under one endpoint's lock, another dropped at some
 * process's exit under none. Two at once on two cores both read the same
 * number and one change is lost. Lose an increment and the region's pages go
 * back to the allocator while a window still draws into them; lose a
 * decrement and they never go back. One core could never see it, and the
 * window is a few instructions wide, which is why nothing had.
 *
 * The same lock the claim takes, held for the check and the change and
 * nothing else - the pages are freed outside it, as they are built outside
 * it - and taken again to release the slot, so a scan on another core sees
 * it free only after everything below has been written.
 */
void memobj_ref(struct memobj *m)
{
    unsigned long flags;

    if (m == NULL) {
        return;
    }

    flags = spin_lock(&objects_lock);

    if (m->in_use) {
        m->refs++;
    }

    spin_unlock(&objects_lock, flags);
}

/*
 * A reference taken only if `m` is still the region `generation` names -
 * for a capability travelling from one table to another, which was resolved
 * against the first and may have been freed, and its slot made a stranger's,
 * before it reaches the second (`install_memory` in `ipc.c`). False, and no
 * reference, when it has gone.
 */
bool memobj_ref_as(struct memobj *m, unsigned generation)
{
    unsigned long flags;
    bool taken = false;

    if (m == NULL) {
        return false;
    }

    flags = spin_lock(&objects_lock);

    if (m->in_use && m->refs > 0 && m->generation == generation) {
        m->refs++;
        taken = true;
    }

    spin_unlock(&objects_lock, flags);
    return taken;
}

void memobj_unref(struct memobj *m)
{
    unsigned long flags;

    if (m == NULL) {
        return;
    }

    flags = spin_lock(&objects_lock);

    if (!m->in_use) {
        spin_unlock(&objects_lock, flags);
        return;
    }

    if (m->refs == 0) {
        spin_unlock(&objects_lock, flags);
        panic("memobj: unreferenced twice");
    }

    m->refs--;

    if (m->refs > 0) {
        spin_unlock(&objects_lock, flags);
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
    spin_unlock(&objects_lock, flags);

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
            if (memobj_page(m, i) != NULL) {      /* given on touch, or not */
                pmm_free_page(memobj_page(m, i));
            }
        }
    }

    free_index(m);

    m->pages = 0;
    release(m);
}

unsigned memobj_in_use(void)
{
    unsigned i;
    unsigned n = 0;

    for (i = 0; i < pool_slots(&objects); i++) {
        if (object_at(i)->in_use) {
            n++;
        }
    }

    return n;
}

unsigned memobj_total(void)
{
    return pool_ceiling(&objects);
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

    /*
     * `virt_to_phys` because this number leaves the address space: it is
     * programmed into a device's descriptor base, and the hardware does not
     * consult a page table on the way. On a board whose kernel reaches RAM
     * through a window rather than an identity map, the pointer the
     * allocator gave and the address the device needs are different numbers
     * (`arch/x86_64/mmu.h`, `PHYS_WINDOW_BASE`).
     */
    return virt_to_phys(memobj_page(m, 0));
}
