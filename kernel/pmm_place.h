/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where the page bitmap goes, as arithmetic rather than as a side effect.
 *
 * **Separated from `pmm.c` for `hal/pc/loader_fb.c`'s reason**: it is a
 * decision the machine this project is aimed at can get wrong, in a way
 * QEMU cannot reproduce, on a boot with no serial port to complain over.
 * The framebuffer decision was pulled out because `-kernel` never exercises
 * it; this one is pulled out because **OVMF reports its largest usable low
 * region at 0x900000 whatever it is given** - measured at ten memory sizes
 * from 512 MB to 32 GB - and the case that breaks is a region beginning
 * *above* the kernel image, which no QEMU configuration here produces.
 *
 * A pure function the host can ask the awkward questions of is worth more
 * than a comment saying it should be fine.
 *
 * The page size is an argument rather than an include, because none of this
 * is architecture: it is where a structure fits inside a range.
 */
#ifndef KOSMOS_KERNEL_PMM_PLACE_H
#define KOSMOS_KERNEL_PMM_PLACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pmm_layout {
    uintptr_t base;         /* first page the bitmap describes */
    size_t    pages;        /* how many pages that is */
    uintptr_t bitmap;       /* where the bitmap itself lives */
    size_t    reserved;     /* pages at the bottom that are not free: the
                             * bitmap, and the image when it is in here */
};

/*
 * False when the bitmap cannot be placed at all, which is a machine whose
 * usable memory is smaller than the structure describing it or does not
 * reach the kernel. The caller panics; there is nothing else to do.
 *
 * `image_end` is where the linker put the last byte of the kernel. It is
 * *not* assumed to be inside the range: on a PC booted through firmware the
 * largest usable block can begin above the image, below it, or around it.
 */
bool pmm_place(uintptr_t image_end, uintptr_t ram_base, uint64_t ram_size,
               size_t page_size, struct pmm_layout *out);

#endif
