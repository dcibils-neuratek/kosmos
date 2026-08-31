/*
 * Memory two processes can both see. See memobj.h for why it is a
 * capability rather than a handle.
 */

#include <stddef.h>
#include <string.h>

#include "memobj.h"
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
        objects[i].base = NULL;
        objects[i].pages = 0;
    }
}

struct memobj *memobj_create(size_t pages)
{
    unsigned i;

    if (pages == 0 || pages > MEMOBJ_PAGES_MAX) {
        return NULL;
    }

    for (i = 0; i < MEMOBJ_MAX; i++) {
        struct memobj *m = &objects[i];

        if (m->in_use) {
            continue;
        }

        m->base = pmm_alloc_contiguous(pages);

        if (m->base == NULL) {
            return NULL;        /* enough memory, perhaps, but not in a run */
        }

        /*
         * Zeroed before anybody can see it. A fresh region holding whatever
         * the last owner left is how one process reads another's memory
         * without either of them doing anything wrong - and this region is
         * about to be handed to a second process on purpose, which makes it
         * the last place to be casual about it.
         */
        memset(m->base, 0, pages * PAGE_SIZE);

        m->in_use = true;
        m->refs = 1;
        m->pages = pages;

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
    m->in_use = false;

    {
        size_t i;

        for (i = 0; i < m->pages; i++) {
            pmm_free_page((char *)m->base + i * PAGE_SIZE);
        }
    }

    m->base = NULL;
    m->pages = 0;
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
