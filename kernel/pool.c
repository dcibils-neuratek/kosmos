/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A pool of kernel objects that grows. `pool.h` is the argument.
 */

#include <stddef.h>
#include <string.h>

#include "pool.h"
#include "pmm.h"
#include "page.h"
#include "panic.h"

/* At least this many objects to a slab, so growing is not a page at a time
 * for small objects nor a single object at a time for large ones. */
#define POOL_SLAB_MIN_OBJECTS 16u

static unsigned slots_of(const struct pool *p)
{
    return __atomic_load_n(&p->slots, __ATOMIC_ACQUIRE);
}

unsigned pool_slots(const struct pool *p)
{
    return slots_of(p);
}

unsigned pool_ceiling(const struct pool *p)
{
    return p->slabs_max * p->per_slab;
}

void *pool_at(const struct pool *p, unsigned i)
{
    return (char *)p->slab[i / p->per_slab] + (size_t)(i % p->per_slab) * p->size;
}

unsigned pool_ceiling_for(size_t each, unsigned floor)
{
    size_t ram = pmm_total_pages() * (size_t)PAGE_SIZE;
    size_t n = ram / each;

    if (n < floor) {
        n = floor;
    }

    if (n > 0x7fffffffu) {
        n = 0x7fffffffu;
    }

    return (unsigned)n;
}

/*
 * Made outside the lock - a slab is up to seventeen pages to allocate and
 * zero, and a lock that masks interrupts is not held across that - and
 * published under it. Two cores that both found their pool full both add a
 * slab; neither is wasted, since slabs are never given back and the next
 * claim uses the other. Only the ceiling, reached in between, sends one back.
 */
bool pool_grow(struct pool *p)
{
    unsigned long flags;
    unsigned made;
    bool added = false;
    char *slab;
    unsigned k;

    if (slots_of(p) >= pool_ceiling(p)) {
        return false;
    }

    slab = pmm_alloc_contiguous(p->slab_pages);

    if (slab == NULL) {
        return false;
    }

    memset(slab, 0, p->slab_pages * PAGE_SIZE);

    if (p->fresh != NULL) {
        for (k = 0; k < p->per_slab; k++) {
            p->fresh(slab + (size_t)k * p->size);
        }
    }

    flags = spin_lock(&p->lock);
    made = slots_of(p);

    if (made < pool_ceiling(p)) {
        p->slab[made / p->per_slab] = slab;
        __atomic_store_n(&p->slots, made + p->per_slab, __ATOMIC_RELEASE);
        added = true;
    }

    spin_unlock(&p->lock, flags);

    if (!added) {
        size_t i;

        for (i = 0; i < p->slab_pages; i++) {
            pmm_free_page(slab + i * PAGE_SIZE);
        }
    }

    return added;
}

void pool_init(struct pool *p, const char *name, size_t size,
               unsigned ceiling, unsigned boot, void (*fresh)(void *object))
{
    size_t dir_pages;

    p->name = name;
    p->size = size;
    p->fresh = fresh;
    p->slots = 0;
    p->lock.locked = 0;
    p->lock.holder = SPIN_NOBODY;
    p->lock.name = name;

    p->slab_pages = (POOL_SLAB_MIN_OBJECTS * size + PAGE_SIZE - 1) / PAGE_SIZE;
    p->per_slab = (unsigned)(p->slab_pages * PAGE_SIZE / size);

    if (ceiling < boot) {
        ceiling = boot;
    }

    p->slabs_max = (ceiling + p->per_slab - 1) / p->per_slab;

    dir_pages = (p->slabs_max * sizeof(void *) + PAGE_SIZE - 1) / PAGE_SIZE;
    p->slab = pmm_alloc_contiguous(dir_pages);

    if (p->slab == NULL) {
        panic("pool: no pages for a pool's directory");
    }

    memset(p->slab, 0, dir_pages * PAGE_SIZE);

    while (slots_of(p) < boot) {
        if (!pool_grow(p)) {
            panic("pool: no pages for a pool's first slabs");
        }
    }
}
