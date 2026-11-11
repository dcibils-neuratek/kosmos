/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Addresses reserved, then protected and released** (`docs/w2.md` part
 * 2, agreed by Diego on 11 November 2026).
 *
 * JavaScriptCore and bmalloc claim gigabytes of addresses at once - the
 * JIT's pool, the Gigacage that keeps an array's index inside a known
 * window - and back pages only as they use them; and they make pages read
 * only, or no access at all, for guards. `SYS_MAP` could do none of it: it
 * gives zeroed pages at once, writable, from a 4 GB window.
 *
 * So a process reserves a range in a window of its own, `USER_RESERVE_VA`
 * up (`process.h`), and the range costs one record and no page. It sets
 * each page's access with `SYS_PROTECT`: a page made read and write is
 * *committed*, and its first touch maps a zeroed page (`reserve_fill`, from
 * `process_fill` as the heap's first touches are); a page made no access
 * gives its page back. `SYS_RELEASE` gives the addresses back, never to be
 * handed out again - a bump pointer, as `SYS_MAP`'s is, with 192 GB of it
 * on the smaller board.
 *
 * **Never writable and executable at once.** There is no such access, and
 * asking for both is `SYS_ERR_FLAGS`. A page may be written and *then* made
 * read and execute, which is how a JIT that flips works; one that keeps two
 * mappings of a region writes through one and runs from the other.
 *
 * **What is held where.** A process's reservations are *spans*: runs of
 * pages with one access, in address order, adjacent ones with the same
 * access merged. A region (`memobj`) given on touch was the other shape,
 * and it keeps an index the size of the region - 64 MB of index for 32 GB
 * of addresses. Which pages are committed is in the page tables and only
 * there, so a span is four words however large it is. The spans come from
 * a pool that grows by slabs and gives nothing back, as every kernel pool
 * does.
 *
 * **One lock**, over every span and over the page-table changes made for
 * one, so a page being re-protected on one core and touched for the first
 * time on another is seen by the second either before or after, never
 * half done. Changing a present page's access is an unmap and a map, and
 * the lock is what makes the moment between them invisible: a fault in it
 * waits here, and then finds the page mapped.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mmu.h"
#include "page.h"
#include "pmm.h"
#include "pool.h"
#include "process.h"
#include "reserve.h"
#include "spinlock.h"
#include "syscall.h"

/* The boot pool, and a slot for every 64 KB of RAM at most: a span is
 * forty bytes, and a process has as many as its reservations have changes
 * of access, which for JavaScriptCore is a few dozen. */
#define SPAN_BOOT_SLOTS 256
#define SPAN_RAM_EACH   (64UL * 1024)

/* 2 MB: the span of one last-level table, which `as_page_entry` answers
 * NULL for when it has not been made - so a walk over 32 GB of reserved and
 * mostly untouched addresses looks at sixteen thousand tables, not eight
 * million pages. The same on both boards. */
#define TABLE_SPAN (512UL * PAGE_SIZE)

static struct pool spans;
static struct spinlock reserve_lock = SPINLOCK("reserve");

void reserve_init(void)
{
    pool_init(&spans, "reserved spans", sizeof(struct reserve_span),
              pool_ceiling_for(SPAN_RAM_EACH, SPAN_BOOT_SLOTS),
              SPAN_BOOT_SLOTS, NULL);
}

/* A free span, taken before anybody can see it free; NULL at the ceiling.
 * Claimed outside `reserve_lock`, since growing the pool takes pages. */
static struct reserve_span *claim(void)
{
    unsigned i;

again:
    for (i = 0; i < pool_slots(&spans); i++) {
        struct reserve_span *s = pool_at(&spans, i);
        unsigned long flags = spin_lock(&reserve_lock);

        if (!s->in_use) {
            memset(s, 0, sizeof(*s));
            s->in_use = true;
            spin_unlock(&reserve_lock, flags);
            return s;
        }

        spin_unlock(&reserve_lock, flags);
    }

    if (pool_grow(&spans)) {
        goto again;
    }

    return NULL;
}

/* Back to the pool; `reserve_lock` is held. */
static void give_back_locked(struct reserve_span *s)
{
    memset(s, 0, sizeof(*s));
}

static void give_back(struct reserve_span *s)
{
    unsigned long flags;

    if (s == NULL) {
        return;
    }

    flags = spin_lock(&reserve_lock);
    give_back_locked(s);
    spin_unlock(&reserve_lock, flags);
}

static uintptr_t span_end(const struct reserve_span *s)
{
    return s->va + s->pages * PAGE_SIZE;
}

static uint64_t attrs_for(unsigned access)
{
    switch (access) {
    case RESERVE_READ:  return MAP_USER_RO;
    case RESERVE_RW:    return MAP_USER_RW;
    case RESERVE_RX:    return MAP_USER_RX;
    default:            return 0;
    }
}

/*
 * The span holding `va`, or NULL. `reserve_lock` is held.
 */
static struct reserve_span *span_at(const struct process *p, uintptr_t va)
{
    struct reserve_span *s;

    for (s = p->spans; s != NULL && s->va <= va; s = s->next) {
        if (va < span_end(s)) {
            return s;
        }
    }

    return NULL;
}

/*
 * A range a process may name: page aligned, not empty, inside the window
 * and below where the next reservation would go - so the multiplication
 * that finds its end cannot wrap.
 */
static bool range_ok(const struct process *p, uintptr_t va, size_t pages)
{
    if ((va & (PAGE_SIZE - 1)) != 0 || pages == 0) {
        return false;
    }

    if (va < USER_RESERVE_VA || va >= p->next_reserve) {
        return false;
    }

    return pages <= (p->next_reserve - va) / PAGE_SIZE;
}

/*
 * Every present page in `[va, va + pages)` given `access`, or given back
 * when it is RESERVE_NONE. `reserve_lock` is held.
 */
static void change_pages(struct process *p, uintptr_t va, size_t pages,
                         unsigned access)
{
    uintptr_t end = va + pages * PAGE_SIZE;

    while (va < end) {
        uintptr_t phys;

        if (as_page_entry(p->space, va) == NULL) {
            va = (va & ~(TABLE_SPAN - 1)) + TABLE_SPAN;     /* no table here */
            continue;
        }

        phys = as_page_phys(p->space, va);

        if (phys != 0) {
            (void)as_unmap(p->space, va, 1);

            if (access == RESERVE_NONE) {
                pmm_free_page(phys_to_virt(phys));
                p->committed_pages--;
            } else {
                (void)as_map(p->space, va, phys, 1, attrs_for(access));
            }
        }

        va += PAGE_SIZE;
    }
}

/*
 * Cuts the span holding `at` in two, so that one of them begins there,
 * using `spare` for the second half. True when `spare` was used.
 * `reserve_lock` is held.
 */
static bool split_at(struct process *p, uintptr_t at, struct reserve_span *spare)
{
    struct reserve_span *s = span_at(p, at);

    if (s == NULL || s->va == at) {
        return false;
    }

    spare->va = at;
    spare->pages = (span_end(s) - at) / PAGE_SIZE;
    spare->access = s->access;
    spare->next = s->next;
    s->pages = (at - s->va) / PAGE_SIZE;
    s->next = spare;
    return true;
}

/* Adjacent spans with one access, made one. `reserve_lock` is held. */
static void merge(struct process *p)
{
    struct reserve_span *s = p->spans;

    while (s != NULL && s->next != NULL) {
        struct reserve_span *n = s->next;

        if (span_end(s) == n->va && s->access == n->access) {
            s->pages += n->pages;
            s->next = n->next;
            give_back_locked(n);
        } else {
            s = n;
        }
    }
}

long reserve_create(struct process *p, size_t pages)
{
    struct reserve_span *s, **link;
    unsigned long flags;
    long va;

    if (pages == 0 || p->next_reserve < USER_RESERVE_VA
        || pages > (USER_RESERVE_END - p->next_reserve) / PAGE_SIZE) {
        return SYS_ERR_NO_ROOM;
    }

    s = claim();

    if (s == NULL) {
        return SYS_ERR_NO_ROOM;
    }

    flags = spin_lock(&reserve_lock);

    /* Checked again under the lock: two threads may reserve at once. */
    if (pages > (USER_RESERVE_END - p->next_reserve) / PAGE_SIZE) {
        give_back_locked(s);
        spin_unlock(&reserve_lock, flags);
        return SYS_ERR_NO_ROOM;
    }

    s->va = p->next_reserve;
    s->pages = pages;
    s->access = RESERVE_NONE;
    s->next = NULL;

    /* At the end: the pointer only rises, so the list stays in order. */
    for (link = &p->spans; *link != NULL; link = &(*link)->next) {
    }
    *link = s;

    p->next_reserve += pages * PAGE_SIZE;
    va = (long)s->va;
    merge(p);
    spin_unlock(&reserve_lock, flags);
    return va;
}

long reserve_protect(struct process *p, uintptr_t va, size_t pages,
                     unsigned long access)
{
    struct reserve_span *a, *b, *s;
    uintptr_t end, at;
    unsigned long flags;

    if (access == RESERVE_RWX || access > RESERVE_RX) {
        return SYS_ERR_FLAGS;           /* never writable and executable */
    }

    if (!range_ok(p, va, pages)) {
        return SYS_ERR_FAULT;
    }

    end = va + pages * PAGE_SIZE;

    /* The two halves a split at each end may need, before the lock. */
    a = claim();
    b = claim();

    if (a == NULL || b == NULL) {
        give_back(a);
        give_back(b);
        return SYS_ERR_NO_ROOM;
    }

    flags = spin_lock(&reserve_lock);

    /* All of it reserved and none of it released: a run of spans with no
     * gap, from `va` to `end`. */
    for (at = va; at < end; ) {
        s = span_at(p, at);

        if (s == NULL) {
            spin_unlock(&reserve_lock, flags);
            give_back(a);
            give_back(b);
            return SYS_ERR_FAULT;
        }

        at = span_end(s);
    }

    if (split_at(p, va, a)) {
        a = NULL;
    }

    if (end < p->next_reserve && split_at(p, end, b)) {
        b = NULL;
    }

    for (s = span_at(p, va); s != NULL && s->va < end; s = s->next) {
        if (s->access != access) {
            change_pages(p, s->va, s->pages, (unsigned)access);
            s->access = (unsigned)access;
        }
    }

    merge(p);

    if (a != NULL) {
        give_back_locked(a);
    }

    if (b != NULL) {
        give_back_locked(b);
    }

    spin_unlock(&reserve_lock, flags);
    return 0;
}

long reserve_release(struct process *p, uintptr_t va, size_t pages)
{
    struct reserve_span *a, *b, **link;
    uintptr_t end;
    unsigned long flags;

    if (!range_ok(p, va, pages)) {
        return SYS_ERR_FAULT;
    }

    end = va + pages * PAGE_SIZE;
    a = claim();
    b = claim();

    if (a == NULL || b == NULL) {
        give_back(a);
        give_back(b);
        return SYS_ERR_NO_ROOM;
    }

    flags = spin_lock(&reserve_lock);

    if (split_at(p, va, a)) {
        a = NULL;
    }

    if (end < p->next_reserve && split_at(p, end, b)) {
        b = NULL;
    }

    /* Every span inside the range: its pages given back, and it gone. A
     * part already released is simply not there - releasing twice is not
     * an error, as unmapping twice is not. */
    link = &p->spans;

    while (*link != NULL && (*link)->va < end) {
        struct reserve_span *s = *link;

        if (s->va >= va) {
            change_pages(p, s->va, s->pages, RESERVE_NONE);
            *link = s->next;
            give_back_locked(s);
        } else {
            link = &s->next;
        }
    }

    if (a != NULL) {
        give_back_locked(a);
    }

    if (b != NULL) {
        give_back_locked(b);
    }

    spin_unlock(&reserve_lock, flags);
    return 0;
}

bool reserve_fill(struct process *p, uintptr_t va)
{
    uintptr_t page = va & ~(uintptr_t)(PAGE_SIZE - 1);
    struct reserve_span *s;
    unsigned long flags;
    bool ok = false;

    flags = spin_lock(&reserve_lock);
    s = span_at(p, page);

    if (s != NULL && s->access != RESERVE_NONE) {
        void *mem;

        if (as_page_phys(p->space, page) != 0) {
            ok = true;                  /* another thread of it, just now */
        } else if (pmm_room_for_user(1) && (mem = pmm_alloc_page()) != NULL) {
            /* Zeroed before the process can see it, as every page is. */
            memset(mem, 0, PAGE_SIZE);

            if (as_map(p->space, page, virt_to_phys(mem), 1,
                       attrs_for(s->access)) == AS_OK) {
                p->committed_pages++;
                ok = true;
            } else {
                pmm_free_page(mem);
            }
        }
    }

    spin_unlock(&reserve_lock, flags);
    return ok;
}

const char *reserve_describe(struct process *p, uintptr_t va)
{
    static const char *const names[] = {
        [RESERVE_NONE] = "no access",
        [RESERVE_READ] = "read only",
        [RESERVE_RW]   = "read and write",
        [RESERVE_RX]   = "read and execute",
    };
    const struct reserve_span *s;
    const char *name = NULL;
    unsigned long flags;

    if (p == NULL || va < USER_RESERVE_VA || va >= USER_RESERVE_END) {
        return NULL;
    }

    flags = spin_lock(&reserve_lock);
    s = span_at(p, va);

    if (s != NULL && s->access <= RESERVE_RX) {
        name = names[s->access];
    }

    spin_unlock(&reserve_lock, flags);
    return name;
}

void reserve_teardown(struct process *p)
{
    unsigned long flags = spin_lock(&reserve_lock);

    while (p->spans != NULL) {
        struct reserve_span *s = p->spans;

        change_pages(p, s->va, s->pages, RESERVE_NONE);
        p->spans = s->next;
        give_back_locked(s);
    }

    p->next_reserve = USER_RESERVE_VA;
    p->committed_pages = 0;
    spin_unlock(&reserve_lock, flags);
}
