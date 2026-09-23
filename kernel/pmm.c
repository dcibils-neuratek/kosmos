/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pmm.h"
#include "mmu.h"
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
static size_t    total;         /* pages the bitmap covers, holes included */

/*
 * **Pages this allocator actually manages, which is not what the bitmap
 * spans.**
 *
 * Since the bitmap covers every usable range in one sweep, it also covers
 * the gaps between them - the PCI hole above all - and those bits are
 * simply never freed. `total` is the span because that is what an index is
 * bounded by; this is the number that means "memory", and it is what gets
 * reported.
 *
 * Keeping one number for both was briefly the arrangement, and a machine
 * with four gigabytes announced 6143 MB of RAM. A number the machine prints
 * about itself has to be true, which is the whole argument in
 * `hal_ram_capped`'s comment applied one layer down.
 */
static size_t    managed;
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
    struct memrange usable[PMM_RANGES_MAX];
    unsigned count;
    unsigned r;
    uintptr_t span_base;
    uintptr_t span_end;
    size_t i;

    struct pmm_layout at;

    hal_ram_range(&ram);

    /*
     * **One bitmap over every usable range, with the gaps between them
     * marked taken.**
     *
     * A PC with four gigabytes or more does not have one block of memory:
     * the PCI hole splits it, a piece below and a larger piece above four
     * gigabytes. Managing only one of them left Diego's ThinkCentre on
     * about three of its eight (`roadmap.md` 5zd-d step four).
     *
     * The cheap answer is the one taken here: span the bitmap from the
     * lowest usable byte to the highest and start with everything used, so
     * a hole costs only the bits nobody ever frees. Nine gigabytes of span
     * at one bit per 4 KB page is 288 KB of bitmap - against the five
     * gigabytes it buys back, that is not a trade anyone has to think
     * about. The alternative, a descriptor per range and a search across
     * them on every allocation, is more code and more state to keep
     * consistent for the same answer.
     *
     * The *bitmap* still goes in the range holding the kernel image, which
     * is what `hal_ram_range` answers and why both calls exist.
     */
    count = hal_ram_ranges(usable, PMM_RANGES_MAX);

    span_base = ram.base;
    span_end  = ram.base + ram.size;

    for (r = 0; r < count; r++) {
        uintptr_t end = usable[r].base + usable[r].size;

        if (usable[r].base < span_base) {
            span_base = usable[r].base;
        }

        if (end > span_end) {
            span_end = end;
        }
    }

    /*
     * The bitmap has to live somewhere and there is no allocator yet to ask
     * - this is the allocator. Where it goes is arithmetic with an awkward
     * case in it, so it lives in `pmm_place.c` where the host can ask it
     * questions this machine's firmware never will.
     *
     * 512 MB of 4 KB pages is 131072 bits, so 16 KB of bitmap. Four pages
     * to describe half a gigabyte.
     */
    if (!pmm_place((uintptr_t)__image_end, span_base, span_end - span_base,
                   PAGE_SIZE, &at)) {
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
    managed   = 0;

    /*
     * **Freed range by range rather than from `at.reserved` upwards.** The
     * span is not all memory: between the ranges are the PCI hole, ACPI
     * tables and whatever else the firmware kept, and a page in one of
     * those is not this allocator's to hand out. Starting from "all used"
     * means a gap needs no code at all - it is simply never freed - and a
     * range this loop somehow missed stays out of circulation, which fails
     * safely.
     *
     * `at.reserved` is the bitmap and the image at the bottom of the span,
     * so a page below it is skipped wherever its range begins.
     */
    for (r = 0; r < count; r++) {
        uintptr_t at_page = usable[r].base;
        uintptr_t end     = usable[r].base + usable[r].size;

        if (at_page < ram_base) {
            at_page = ram_base;
        }

        for (; at_page + PAGE_SIZE <= end; at_page += PAGE_SIZE) {
            size_t index = (at_page - ram_base) / PAGE_SIZE;

            if (index < total) {
                managed++;

                if (index >= at.reserved) {
                    set_free(index);
                }
            }
        }
    }

    /*
     * A board with no `hal_ram_ranges` answer at all is one this kernel
     * would give no memory, so the range it was loaded into is freed the
     * old way. Nothing reaches this today; it is here so that adding a
     * board cannot silently produce a machine with no free pages.
     */
    if (count == 0) {
        managed = total;

        for (i = at.reserved; i < total; i++) {
            set_free(i);
        }
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
        return phys_to_virt(ram_base + i * PAGE_SIZE);
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
        return phys_to_virt(ram_base + start * PAGE_SIZE);
    }

    spin_unlock(&pmm_lock, flags);
    return NULL;
}

void pmm_free_page(void *page)
{
    unsigned long flags;
    uintptr_t addr = virt_to_phys(page);
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
    return managed;
}

/*
 * **The reserve: memory a program may not take.**
 *
 * Every per-process cap on memory has gone (`threads.md` step 1b): a program
 * may map as much as the machine has, because a number chosen in advance is
 * wrong on every machine but the one it was chosen for. What stops the last
 * page going to a runaway is this instead - a slice at the bottom that only
 * the kernel's own allocations may reach: a thread's stacks, page tables, a
 * pool's slab, and a process being started, which is how somebody opens
 * Processes to end the runaway.
 *
 * A thirty-second of memory, between 8 MB and 256 MB: 16 MB on this 512 MB
 * board, which is five processes' worth, and 256 MB on a 16 GB machine.
 */
size_t pmm_reserve_pages(void)
{
    size_t pages = total / 32u;
    size_t least = (8u * 1024u * 1024u) / PAGE_SIZE;
    size_t most = (256u * 1024u * 1024u) / PAGE_SIZE;

    if (pages < least) {
        pages = least;
    }

    if (pages > most) {
        pages = most;
    }

    return pages;
}

/*
 * Whether `pages` can be given to a program without eating the reserve.
 *
 * Asked before the allocation rather than enforced inside it, so that a
 * refusal is one clean answer at the syscall instead of a request half
 * served. Two callers at once can both be told yes and both allocate, which
 * takes the reserve a little below where it was meant to be; the slack is
 * megabytes and the alternative is a lock around every page.
 */
bool pmm_room_for_user(size_t pages)
{
    size_t reserve = pmm_reserve_pages();

    return freecount > reserve && freecount - reserve >= pages;
}
