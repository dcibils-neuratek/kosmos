#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pmm.h"
#include "panic.h"
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

void pmm_init(void)
{
    struct memrange ram;
    uintptr_t bitmap_start;
    uintptr_t first_free;
    size_t i;

    hal_ram_range(&ram);

    ram_base = ram.base;
    total    = ram.size / PAGE_SIZE;

    /*
     * The bitmap has to live somewhere, and there is no allocator yet to ask
     * — this is the allocator. It goes directly after the kernel image, and
     * the pages it occupies are then marked used like any others.
     *
     * 512 MB of 4 KB pages is 131072 bits, so 16 KB of bitmap. Four pages to
     * describe half a gigabyte.
     */
    bitmap_start = PAGE_ALIGN_UP((uintptr_t)__image_end);
    bitmap       = (uint64_t *)bitmap_start;

    /*
     * Everything starts used, and the pages above the bitmap are then handed
     * back. Starting from "all used" rather than "all free" means a page
     * that is somehow missed stays out of circulation, which fails safely.
     */
    for (i = 0; i < bitmap_words(); i++) {
        bitmap[i] = 0;
    }
    freecount = 0;

    first_free = PAGE_ALIGN_UP(bitmap_start + bitmap_words() * sizeof(uint64_t));

    if (first_free < ram_base || (first_free - ram_base) / PAGE_SIZE >= total) {
        panic("pmm_init: the kernel image does not fit in RAM");
    }

    for (i = (first_free - ram_base) / PAGE_SIZE; i < total; i++) {
        set_free(i);
    }
}

void *pmm_alloc_page(void)
{
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
        return (void *)(ram_base + i * PAGE_SIZE);
    }

    return NULL;
}

void pmm_free_page(void *page)
{
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

    if (page_is_free(i)) {
        panic("pmm_free_page: double free");
    }

    set_free(i);

    /* Freeing below the cursor would otherwise leave the page invisible
     * until the scan wraps all the way around. */
    if (i / 64 < cursor) {
        cursor = i / 64;
    }
}

size_t pmm_free_pages(void)
{
    return freecount;
}

size_t pmm_total_pages(void)
{
    return total;
}
