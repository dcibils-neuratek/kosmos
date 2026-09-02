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
        objects[i].pages = 0;
        objects[i].indexes = 0;
    }
}

void *memobj_page(const struct memobj *m, size_t i)
{
    if (m == NULL || i >= m->pages) {
        return NULL;
    }

    return m->index[i / MEMOBJ_PER_INDEX][i % MEMOBJ_PER_INDEX];
}

/* Everything a half-built region has taken, given back. */
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
}

struct memobj *memobj_create(size_t pages)
{
    unsigned i;

    if (pages == 0 || pages > MEMOBJ_PAGES_MAX) {
        return NULL;
    }

    for (i = 0; i < MEMOBJ_MAX; i++) {
        struct memobj *m = &objects[i];
        size_t indexes = (pages + MEMOBJ_PER_INDEX - 1) / MEMOBJ_PER_INDEX;
        size_t k, n;

        if (m->in_use) {
            continue;
        }

        /*
         * The index first, because it is small and failing here costs
         * nothing to undo. Each index page holds 512 page pointers.
         */
        m->pages = pages;
        m->indexes = 0;

        for (k = 0; k < indexes; k++) {
            m->index[k] = pmm_alloc_page();

            if (m->index[k] == NULL) {
                unwind(m, 0);
                return NULL;
            }

            m->indexes = k + 1;
        }

        /*
         * Then the pages themselves, one at a time and in no particular
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

        m->in_use = true;
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
    m->in_use = false;

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
