/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Whether a blob is still the bytes the build put in it.
 *
 * **Separated from `main.c` for the reason `pmm_place.h` and
 * `hal/pc/loader_fb.c` were separated**, and it is the same reason three
 * times now: this decides something the machine this project is aimed at
 * gets wrong in a way QEMU cannot reproduce, on a boot with no serial port
 * to complain over.
 *
 * What it is for: every process on this machine runs out of one blob.
 * `process_create` maps the read-only half of the init image straight out of
 * the kernel's own copy - one set of physical pages for every address space
 * - so the code, the fonts and the Lua source of every program are these
 * bytes and no others. No process can write one: the mapping is read only,
 * and the pages sit below the region the allocator hands out, so nothing the
 * kernel allocates can land on them either.
 *
 * On the ThinkPad they change anyway, and the only symptom is a Lua syntax
 * error in whichever server parses first. `tools/bin2c.py` writes down what
 * it emitted; this asks, at chosen moments of the boot, whether that is
 * still what is there.
 *
 * FNV-1a, which is four lines and needs no table. A canary rather than a
 * signature: nothing here is defending against an adversary choosing the
 * bytes, only against memory that stopped holding them.
 *
 * The page size is an argument rather than an include, because none of this
 * is architecture: it is a hash over a range.
 */
#ifndef KOSMOS_KERNEL_IMAGE_SUM_H
#define KOSMOS_KERNEL_IMAGE_SUM_H

#include <stdbool.h>
#include <stddef.h>

#define IMAGE_SUM_FNV64_BASIS   0xcbf29ce484222325UL
#define IMAGE_SUM_FNV64_PRIME   0x100000001b3UL
#define IMAGE_SUM_FNV32_BASIS   0x811c9dc5u
#define IMAGE_SUM_FNV32_PRIME   0x01000193u

static inline unsigned long image_sum64(const unsigned char *at,
                                        unsigned long bytes)
{
    unsigned long h = IMAGE_SUM_FNV64_BASIS;
    unsigned long i;

    for (i = 0; i < bytes; i++) {
        h = (h ^ at[i]) * IMAGE_SUM_FNV64_PRIME;
    }

    return h;
}

static inline unsigned image_sum32(const unsigned char *at,
                                   unsigned long bytes)
{
    unsigned h = IMAGE_SUM_FNV32_BASIS;
    unsigned long i;

    for (i = 0; i < bytes; i++) {
        h = (h ^ at[i]) * IMAGE_SUM_FNV32_PRIME;
    }

    return h;
}

/*
 * What a walk of the page table found.
 *
 * `first` is only meaningful when `bad` is not zero, and it is an index
 * rather than an address: the caller knows where the blob lives and the
 * multiplication belongs where the printing is.
 *
 * A short last page is hashed over the bytes it has, never over the padding
 * after them - which is the one case a test has to ask about, since the
 * blobs this runs on are all several megabytes and none of them happen to
 * be a whole number of pages.
 */
/*
 * **The list, not only the first.**
 *
 * One address says a page is wrong. *Which* pages are wrong says what kind
 * of fault it is, and that is the question: eleven contiguous pages are a
 * block that landed in the wrong place, eleven scattered ones are not, and
 * eleven that move between two boots of the same stick are not a placement
 * fault at all. None of those can be told apart from a single number.
 *
 * Sixteen kept, which fits on a line of a boot screen somebody photographs.
 * `bad` is the true count either way, so a longer list says it is longer
 * rather than looking like all there was.
 */
#define IMAGE_SUM_LISTED 16

struct image_sum_walk {
    unsigned long bad;      /* how many pages differ, in total */
    unsigned long first;    /* the index of the first that does */
    unsigned long listed;   /* how many of them are in `at_page` */
    unsigned long at_page[IMAGE_SUM_LISTED];
};

static inline struct image_sum_walk
image_sum_walk(const unsigned char *at, unsigned long bytes,
               unsigned long page_bytes, unsigned long pages,
               const unsigned *page_sum)
{
    struct image_sum_walk out;
    unsigned long page;

    out.bad = 0;
    out.first = 0;
    out.listed = 0;

    for (page = 0; page < IMAGE_SUM_LISTED; page++) {
        out.at_page[page] = 0;
    }

    for (page = 0; page < pages; page++) {
        unsigned long start = page * page_bytes;
        unsigned long n;

        if (start >= bytes) {
            break;
        }

        n = bytes - start;

        if (n > page_bytes) {
            n = page_bytes;
        }

        if (image_sum32(at + start, n) != page_sum[page]) {
            if (out.bad == 0) {
                out.first = page;
            }

            if (out.listed < IMAGE_SUM_LISTED) {
                out.at_page[out.listed] = page;
                out.listed++;
            }

            out.bad++;
        }
    }

    return out;
}

#endif
