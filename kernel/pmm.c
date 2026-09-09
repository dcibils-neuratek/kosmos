/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pmm.h"
#include "pmm_place.h"
#include "panic.h"
#include "spinlock.h"
#include "page.h"
#include "hal.h"

/* The end of everything the linker placed, including the boot stack. The
 * bitmap goes immediately after it. */
extern char __image_end[];

/*
 * A set bit means the page is free.
 *
 * That way finding one is a search for a non-zero word followed by a count
 * of trailing zeros, which is a single instruction, instead of a search for
 * a zero bit inside a word of ones.
 */
static uint64_t *bitmap;
static size_t    total;         /* pages the bitmap covers */
static size_t    freecount;
static uintptr_t ram_base;

/* Where the last allocation came from. Without it every allocation rescans
 * from page zero, and the kernel's own pages are always at the bottom, so
 * the scan gets longer as memory fills. */
static size_t    cursor;

static inline size_t bitmap_words(void)
{
    return (total + 63) / 64;
}

static inline bool page_is_free(size_t i)
{
    return (bitmap[i / 64] >> (i % 64)) & 1;
}

static inline void set_free(size_t i)
{
    bitmap[i / 64] |= (uint64_t)1 << (i % 64);
    freecount++;
}

static inline void set_used(size_t i)
{
    bitmap[i / 64] &= ~((uint64_t)1 << (i % 64));
    freecount--;
}

uintptr_t pmm_ram_base(void)
{
    return ram_base;
}

void pmm_init(void)
{
    struct memrange ram;
    size_t i;

    struct pmm_layout at;

    hal_ram_range(&ram);

    /*
     * The bitmap has to live somewhere and there is no allocator yet to ask
     * - this is the allocator. Where it goes is arithmetic with an awkward
     * case in it, so it lives in `pmm_place.c` where the host can ask it
     * questions this machine's firmware never will.
     *
     * 512 MB of 4 KB pages is 131072 bits, so 16 KB of bitmap. Four pages
     * to describe half a gigabyte.
     */
    if (!pmm_place((uintptr_t)__image_end, ram.base, ram.size, PAGE_SIZE,
                   &at)) {
        panic("pmm_init: no room for the page bitmap in the memory the "
              "board reported");
    }

    ram_base = at.base;
    total    = at.pages;
    bitmap   = (uint64_t *)at.bitmap;

    /*
     * Everything starts used, and the pages above the bitmap are then handed
     * back. Starting from "all used" rather than "all free" means a page
     * that is somehow missed stays out of circulation, which fails safely.
     */
    for (i = 0; i < bitmap_words(); i++) {
        bitmap[i] = 0;
    }
    freecount = 0;

    for (i = at.reserved; i < total; i++) {
        set_free(i);
    }
}

/*
 * The bitmap's lock.
 *
 * **One lock for the whole allocator, and that is the right grain here.**
 * The bitmap is one array, the cursor is one word, and the free count is one
 * number - there is nothing to partition. Per-CPU free lists are the usual
 * next step and they are an optimisation for contention that does not exist
 * yet: a page is allocated when a process starts or a region is made, not on
 * any hot path. `docs/smp.md` names it as the thing to do if this ever shows
 * up in a profile.
 *
 * Held across the scan *and* the `set_used`, because those two together are
 * the claim - the same window `alloc_thread` had.
 */
static struct spinlock pmm_lock = SPINLOCK("pmm");

void *pmm_alloc_page(void)
{
    unsigned long flags = spin_lock(&pmm_lock);
    size_t words = bitmap_words();
    size_t n;

    for (n = 0; n < words; n++) {
        size_t w = (cursor + n) % words;
        size_t i;

        if (bitmap[w] == 0) {
            continue;
        }

        i = w * 64 + (size_t)__builtin_ctzll(bitmap[w]);

        /* The bits past `total` in the last word are never set free, so this
         * cannot trigger. It is here because if it ever does, handing out a
         * page beyond the end of RAM is a fault a long way from its cause. */
        if (i >= total) {
            panic("pmm_alloc_page: a free bit past the end of RAM");
        }

        set_used(i);
        cursor = w;
        spin_unlock(&pmm_lock, flags);
        return (void *)(ram_base + i * PAGE_SIZE);
    }

    spin_unlock(&pmm_lock, flags);
    return NULL;
}

void *pmm_alloc_contiguous(size_t count)
{
    unsigned long flags;
    size_t start;
    size_t i;

    if (count == 0) {
        return NULL;
    }

    if (count == 1) {
        return pmm_alloc_page();
    }

    /*
     * A linear scan for a run of free bits. No cursor and no cleverness: this
     * is called a handful of times at boot and never on a hot path, and a
     * first-fit scan over a bitmap is the easiest thing to reason about when
     * it goes wrong.
     */
    flags = spin_lock(&pmm_lock);

    for (start = 0; start + count <= total; start++) {
        if (!page_is_free(start)) {
            continue;
        }

        for (i = 1; i < count; i++) {
            if (!page_is_free(start + i)) {
                break;
            }
        }

        if (i < count) {
            /* Resume past the page that broke the run, not at start + 1. */
            start += i;
            continue;
        }

        for (i = 0; i < count; i++) {
            set_used(start + i);
        }

        spin_unlock(&pmm_lock, flags);
        return (void *)(ram_base + start * PAGE_SIZE);
    }

    spin_unlock(&pmm_lock, flags);
    return NULL;
}

void pmm_free_page(void *page)
{
    unsigned long flags;
    uintptr_t addr = (uintptr_t)page;
    size_t i;

    if ((addr & PAGE_MASK) != 0) {
        panic("pmm_free_page: address is not page aligned");
    }

    if (addr < ram_base) {
        panic("pmm_free_page: address is below RAM");
    }

    i = (addr - ram_base) / PAGE_SIZE;

    if (i >= total) {
        panic("pmm_free_page: address is past the end of RAM");
    }

    /*
     * The checks above are on the address and need no lock; from here it is
     * the bitmap.
     *
     * The double-free check is inside, because it is a *read of the bitmap*
     * and the panic it raises has to be true - reading it unlocked would let
     * two cores freeing two different pages in the same word race the
     * read-modify-write in `set_free` and lose one of them.
     */
    flags = spin_lock(&pmm_lock);

    if (page_is_free(i)) {
        spin_unlock(&pmm_lock, flags);
        panic("pmm_free_page: double free");
    }

    set_free(i);

    /* Freeing below the cursor would otherwise leave the page invisible
     * until the scan wraps all the way around. */
    if (i / 64 < cursor) {
        cursor = i / 64;
    }

    spin_unlock(&pmm_lock, flags);
}

size_t pmm_free_pages(void)
{
    return freecount;
}

size_t pmm_total_pages(void)
{
    return total;
}
