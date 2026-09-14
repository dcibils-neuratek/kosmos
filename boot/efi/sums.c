/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Whether a file read off a stick is the file the build wrote. `sums.h` has
 * the file's layout and why it exists.
 */
#include "sums.h"

static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }

    return v;
}

uint64_t sums_fnv(const uint8_t *bytes, uint64_t size)
{
    uint64_t h = 0xcbf29ce484222325ull;
    uint64_t i;

    for (i = 0; i < size; i++) {
        h = (h ^ bytes[i]) * 0x100000001b3ull;
    }

    return h;
}

void sums_check(const uint8_t *data, uint64_t size,
                const uint8_t *sums, uint64_t sums_size,
                struct sums_result *out)
{
    uint64_t page;

    out->verdict = SUMS_MALFORMED;
    out->pages = (size + SUMS_PAGE - 1) / SUMS_PAGE;
    out->wrong = 0;
    out->first = 0;
    out->built = 0;

    if (sums == NULL || sums_size < SUMS_HEADER
        || get64(sums) != SUMS_MAGIC || get64(sums + 16) != SUMS_PAGE) {
        return;
    }

    out->built = get64(sums + 8);

    if (out->built != size) {
        out->verdict = SUMS_SIZE;
        return;
    }

    /* The right size of file, and sums for some other number of pages. */
    if ((sums_size - SUMS_HEADER) % 8u != 0
        || (sums_size - SUMS_HEADER) / 8u != out->pages) {
        return;
    }

    for (page = 0; page < out->pages; page++) {
        uint64_t left = size - page * SUMS_PAGE;
        uint64_t n = left < SUMS_PAGE ? left : SUMS_PAGE;

        if (sums_fnv(data + page * SUMS_PAGE, n)
            != get64(sums + SUMS_HEADER + page * 8u)) {
            if (out->wrong == 0) {
                out->first = page;
            }

            out->wrong++;
        }
    }

    out->verdict = out->wrong == 0 ? SUMS_SAME : SUMS_DIFFER;
}
