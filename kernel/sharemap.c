/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A process's share window, recorded. `sharemap.h` is the argument.
 */

#include <stddef.h>
#include <string.h>

#include "sharemap.h"
#include "memobj.h"
#include "page.h"
#include "pool.h"
#include "process.h"
#include "spinlock.h"

/* At boot, a few for every server that maps its clients' rings; after
 * that, one for every page of memory at most. */
#define SHAREMAP_BOOT_SLOTS 128u
#define SHAREMAP_RAM_EACH   PAGE_SIZE

static struct pool maps;

/*
 * Guards every record's `in_use` and every process's list. One lock rather
 * than one a process because a process's threads map from several cores at
 * once, and the claim below scans records that any process may be taking.
 * Held for a walk and a link, never for page-table work: that is the
 * caller's, outside it.
 */
static struct spinlock maps_lock = SPINLOCK("share maps");

void sharemap_init(void)
{
    pool_init(&maps, "share maps", sizeof(struct sharemap),
              pool_ceiling_for(SHAREMAP_RAM_EACH, SHAREMAP_BOOT_SLOTS),
              SHAREMAP_BOOT_SLOTS, NULL);
}

/* A free record, marked taken before anybody else can see it free; NULL at
 * the ceiling. The same shape as `memobj_create`'s claim. */
static struct sharemap *claim(void)
{
    unsigned i;

again:
    for (i = 0; i < pool_slots(&maps); i++) {
        struct sharemap *s = pool_at(&maps, i);
        unsigned long flags = spin_lock(&maps_lock);

        if (!s->in_use) {
            memset(s, 0, sizeof(*s));
            s->in_use = true;
            spin_unlock(&maps_lock, flags);
            return s;
        }

        spin_unlock(&maps_lock, flags);
    }

    if (pool_grow(&maps)) {
        goto again;
    }

    return NULL;
}

static void give_back(struct sharemap *s)
{
    unsigned long flags = spin_lock(&maps_lock);

    memset(s, 0, sizeof(*s));
    spin_unlock(&maps_lock, flags);
}

struct sharemap *sharemap_place(struct process *p, size_t pages,
                                struct memobj *region)
{
    struct sharemap *s;
    struct sharemap **link;
    uintptr_t at = USER_SHARE_VA;
    size_t bytes;
    unsigned long flags;

    if (pages == 0 || pages > (USER_SHARE_END - USER_SHARE_VA) / PAGE_SIZE) {
        return NULL;
    }

    s = claim();

    if (s == NULL) {
        return NULL;
    }

    bytes = pages * PAGE_SIZE;
    flags = spin_lock(&maps_lock);

    /*
     * **The lowest gap that fits.** The list is in address order and `at`
     * is where the one before it ends, so a gap is the distance to the next
     * one's start - which is never negative, since nothing overlaps.
     */
    for (link = &p->shares; *link != NULL; link = &(*link)->next) {
        if ((*link)->va - at >= bytes) {
            break;
        }

        at = (*link)->va + (*link)->pages * PAGE_SIZE;
    }

    if (at + bytes > USER_SHARE_END) {
        spin_unlock(&maps_lock, flags);
        give_back(s);
        return NULL;
    }

    s->va = at;
    s->pages = pages;
    s->region = region;
    s->next = *link;
    *link = s;

    spin_unlock(&maps_lock, flags);
    return s;
}

void sharemap_ready(struct sharemap *s)
{
    unsigned long flags = spin_lock(&maps_lock);

    s->ready = true;
    spin_unlock(&maps_lock, flags);
}

void sharemap_forget(struct process *p, struct sharemap *s)
{
    struct sharemap **link;
    unsigned long flags = spin_lock(&maps_lock);

    for (link = &p->shares; *link != NULL; link = &(*link)->next) {
        if (*link == s) {
            *link = s->next;
            break;
        }
    }

    spin_unlock(&maps_lock, flags);
    give_back(s);
}

bool sharemap_take(struct process *p, uintptr_t va, size_t pages,
                   struct memobj **region)
{
    struct sharemap **link;
    struct sharemap *s = NULL;
    unsigned long flags = spin_lock(&maps_lock);

    for (link = &p->shares; *link != NULL && (*link)->va <= va;
         link = &(*link)->next) {
        if ((*link)->va == va) {
            s = *link;
            break;
        }
    }

    /* Exactly one mapping, whole and finished. Part of one would leave a
     * record describing pages that are no longer there. */
    if (s == NULL || s->pages != pages || !s->ready) {
        spin_unlock(&maps_lock, flags);
        return false;
    }

    *link = s->next;
    *region = s->region;
    spin_unlock(&maps_lock, flags);

    give_back(s);
    return true;
}

void sharemap_release_all(struct process *p)
{
    struct sharemap *s;
    unsigned long flags = spin_lock(&maps_lock);

    s = p->shares;
    p->shares = NULL;
    spin_unlock(&maps_lock, flags);

    while (s != NULL) {
        struct sharemap *next = s->next;

        if (s->region != NULL) {
            memobj_unref(s->region);
        }

        give_back(s);
        s = next;
    }
}

unsigned sharemap_in_use(void)
{
    unsigned i, n = 0;

    for (i = 0; i < pool_slots(&maps); i++) {
        if (((struct sharemap *)pool_at(&maps, i))->in_use) {
            n++;
        }
    }

    return n;
}
