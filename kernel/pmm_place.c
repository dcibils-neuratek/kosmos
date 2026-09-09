/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * See `pmm_place.h` for why this is a file of its own.
 */

#include "pmm_place.h"

static uintptr_t align_up(uintptr_t at, size_t to)
{
    return (at + (uintptr_t)to - 1) & ~((uintptr_t)to - 1);
}

bool pmm_place(uintptr_t image_end, uintptr_t ram_base, uint64_t ram_size,
               size_t page_size, struct pmm_layout *out)
{
    uintptr_t base;
    uintptr_t end;
    uintptr_t bitmap;
    uintptr_t first_free;
    size_t pages;
    size_t words;

    if (page_size == 0 || (page_size & (page_size - 1)) != 0
        || ram_size < page_size) {
        return false;
    }

    /*
     * A range the firmware gave is not obliged to be page aligned, and the
     * whole allocator counts pages from its base - so the base moves up and
     * the size comes down with it, rather than the arithmetic being wrong
     * by part of a page for the life of the machine.
     */
    base = align_up(ram_base, page_size);

    if (base < ram_base || base - ram_base >= ram_size) {
        return false;                   /* the range is under a page long */
    }

    end = (uintptr_t)(ram_base + ram_size);
    end &= ~((uintptr_t)page_size - 1);

    if (end <= base) {
        return false;
    }

    pages = (size_t)((end - base) / page_size);
    words = (pages + 63) / 64;

    /*
     * **Immediately after the image, or at the bottom of the range - which
     * ever is higher.**
     *
     * It used to be unconditionally after the image, which is right for
     * every machine where the largest usable block is the one the kernel
     * was loaded into. That is what QEMU's `-kernel` gives and it is not
     * what firmware promises: a PC's low memory is carved up by whatever
     * the firmware kept for itself, and the biggest piece left can begin
     * above the image entirely. The old code caught that with `first_free <
     * ram_base` and panicked - `pmm_init: the kernel image does not fit in
     * RAM` - on a machine where the memory is fine and only the bitmap was
     * in the wrong place.
     */
    bitmap = align_up(image_end, page_size);

    if (bitmap < base) {
        bitmap = base;
    }

    if (bitmap >= end) {
        return false;                   /* the image is above this range */
    }

    first_free = align_up(bitmap + (uintptr_t)words * sizeof(uint64_t),
                          page_size);

    if (first_free <= bitmap || first_free >= end) {
        return false;                   /* no room for the bitmap itself */
    }

    out->base = base;
    out->pages = pages;
    out->bitmap = bitmap;
    out->reserved = (size_t)((first_free - base) / page_size);

    return true;
}
