#ifndef KERNEL_MEMOBJ_H
#define KERNEL_MEMOBJ_H

#include <stdbool.h>
#include <stddef.h>

#include "page.h"           /* PAGE_SIZE: an index page holds 512 pointers */

/*
 * Memory two processes can both see.
 *
 * The one thing IPC cannot do. A message is 2048 bytes, so a window's pixels
 * cannot travel in one - which is why the compositor owns them and an
 * application sends drawing commands instead. That is the right arrangement
 * for a window of widgets and the wrong one for a video frame or a rendered
 * scene, where the pixels change wholesale thirty times a second and
 * describing them costs more than copying them.
 *
 * `gfx.md` 19.4 is the design: a region both sides map, double buffered so
 * neither has to lock, and an explicit commit carrying a damage rectangle.
 * This file is the kernel half - the region itself.
 *
 *--------------------------------------------------------------------------
 * Why it is a capability and not a handle.
 *
 * The obvious shape is a number: create a region, get an id, tell the other
 * process the id. That is a global name, and `CLAUDE.md` forbids it for the
 * reason this system exists - a number anybody can say is a number anybody
 * can guess, and then "you cannot name what you were not given" is no longer
 * true of the one thing in the system that is somebody else's memory.
 *
 * So a region is reached through the capability table, exactly as an
 * endpoint is, and it travels the same way: in a message, translated once by
 * the kernel, arriving as the receiver's own index for the same object. A
 * process that was not handed one cannot map it and cannot refer to it.
 *
 *--------------------------------------------------------------------------
 * Why the pages are *not* contiguous any more.
 *
 * They were, and this comment used to explain why: one run rather than a
 * list keeps mapping simple and keeps the descriptor small, and it named the
 * cost honestly - "a large region can fail to allocate on a fragmented
 * machine even when there is enough memory. That is real."
 *
 * It became real. A PDF page rendered offscreen wants about 730 pages, its
 * content buffers 48, a font program 5 to 13, and the window's own double
 * buffer around 920 - all live at once. Seven megabytes on a 512 MB machine,
 * and whichever was asked for last failed for want of a *run* rather than
 * for want of pages. Reordering only moved which one failed.
 *
 * So a region is a list now, and the objection the old comment raised is
 * answered rather than ignored: **the list does not live in .bss.** A page
 * holds 512 pointers, so a region's pages are indexed by up to eight pages
 * taken from the allocator itself, and the descriptor carries eight pointers
 * instead of one. That is 256 descriptors at 96 bytes rather than 40 - 24 KB
 * of .bss instead of 10 - and the quarter of a megabyte the old comment
 * feared never appears, because the index is allocated only for regions that
 * exist.
 *
 * What is given up: mapping walks an index instead of adding to a base, and
 * a region is no longer a single physical run, so it could not be handed to
 * a device expecting one. Nothing does - DMA in this kernel uses kernel
 * buffers, which are identity mapped and contiguous by construction - and
 * the day something needs it, it needs a different allocator rather than
 * this constraint back.
 */

/*
 * Two hundred and fifty-six, because a descriptor is forty bytes and a
 * tiled image wants one per tile.
 *
 * It was sixteen, which was enough for a few shared surfaces and nothing
 * else. A hundred-megabyte image held as one region cannot work here - the
 * pages are contiguous, for the reason above, and a run of twenty-five
 * thousand of them fails on any machine that has been up for a while. Held
 * as tiles it works, and a tile is an ordinary region. So the pool has to
 * be able to hold a hundred of them.
 *
 * The cost is ten kilobytes of .bss for descriptors that are usually empty,
 * which is the trade this kernel makes everywhere: a fixed shape that fails
 * at a known limit.
 */
#define MEMOBJ_MAX        256
#define MEMOBJ_PAGES_MAX  4096      /* 16 MB, a double-buffered full screen */

/* A page of pointers, and how many such pages the largest region needs. */
#define MEMOBJ_PER_INDEX  (PAGE_SIZE / sizeof(void *))
#define MEMOBJ_INDEXES    ((MEMOBJ_PAGES_MAX + MEMOBJ_PER_INDEX - 1) \
                           / MEMOBJ_PER_INDEX)

struct memobj {
    bool     in_use;
    unsigned generation;            /* against a stale capability */
    unsigned refs;                  /* how many capability slots hold it */

    /*
     * The pages, indexed rather than contiguous. `index[k]` is a page from
     * the allocator holding up to 512 page pointers; page `i` of the region
     * is `index[i / 512][i % 512]`. Use `memobj_page` rather than reaching
     * in, so the arithmetic lives in one place.
     */
    void   **index[MEMOBJ_INDEXES];
    size_t   indexes;               /* how many of the above are in use */
    size_t   pages;
};

void memobj_init(void);

/*
 * A new region of `pages` pages, zeroed, with one reference. NULL when the
 * pool is full or the pages are not there.
 */
struct memobj *memobj_create(size_t pages);

/*
 * Page `i` of the region, or NULL if there is no such page.
 *
 * The one place that knows how a region is laid out. Everything that maps a
 * region walks this rather than adding to a base - which is the whole of
 * what changed when regions stopped being contiguous.
 */
void *memobj_page(const struct memobj *m, size_t i);

/* Reference counting. A region's pages go back when the last capability to
 * it does. */
void memobj_ref(struct memobj *m);
void memobj_unref(struct memobj *m);

/* How many are in use, for the machine's own report. */
unsigned memobj_in_use(void);
unsigned memobj_total(void);

#endif /* KERNEL_MEMOBJ_H */
