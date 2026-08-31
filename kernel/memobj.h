#ifndef KERNEL_MEMOBJ_H
#define KERNEL_MEMOBJ_H

#include <stdbool.h>
#include <stddef.h>

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
 * Why the pages are contiguous.
 *
 * A region is one run of physical pages rather than a list of them, which
 * makes mapping it a loop over a base address instead of a page table full
 * of scattered entries - and, more to the point, keeps the object small.
 * Sixteen regions holding a list of two thousand page pointers each would be
 * a quarter of a megabyte of kernel .bss for something that is usually
 * empty.
 *
 * The cost is that a large region can fail to allocate on a fragmented
 * machine even when there is enough memory. That is real, and it is the
 * trade this kernel makes everywhere: a fixed shape that fails at a known
 * limit beats a flexible one that fails somewhere unpredictable.
 */

#define MEMOBJ_MAX        16
#define MEMOBJ_PAGES_MAX  4096      /* 16 MB, a double-buffered full screen */

struct memobj {
    bool     in_use;
    unsigned generation;            /* against a stale capability */
    unsigned refs;                  /* how many capability slots hold it */
    void    *base;                  /* contiguous, identity mapped */
    size_t   pages;
};

void memobj_init(void);

/*
 * A new region of `pages` pages, zeroed, with one reference. NULL when the
 * pool is full or the pages are not there.
 */
struct memobj *memobj_create(size_t pages);

/* Reference counting. A region's pages go back when the last capability to
 * it does. */
void memobj_ref(struct memobj *m);
void memobj_unref(struct memobj *m);

/* How many are in use, for the machine's own report. */
unsigned memobj_in_use(void);
unsigned memobj_total(void);

#endif /* KERNEL_MEMOBJ_H */
