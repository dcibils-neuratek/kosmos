/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Which programs read and wrote a drive: `progcount.h` says what and why.
 */

#include <stdlib.h>
#include <string.h>

#include "progcount.h"

static struct disk_program *row_for(struct progcount *t, uint32_t process)
{
    unsigned i;

    for (i = 0; i < t->count; i++) {
        if (t->rows[i].process == process) {
            return &t->rows[i];
        }
    }

    if (t->count == t->room) {
        unsigned room = t->room ? t->room * 2u : 8u;
        struct disk_program *grown = realloc(t->rows, room * sizeof(*grown));

        if (grown == NULL) {
            return NULL;
        }

        t->rows = grown;
        t->room = room;
    }

    memset(&t->rows[t->count], 0, sizeof(t->rows[t->count]));
    t->rows[t->count].process = process;
    return &t->rows[t->count++];
}

void progcount_note(struct progcount *t, uint32_t process, bool wrote,
                    uint64_t bytes, const char *path)
{
    struct disk_program *r = row_for(t, process);
    size_t n;

    if (r == NULL) {
        return;
    }

    if (wrote) {
        r->writes++;
        r->write_bytes += bytes;
    } else {
        r->reads++;
        r->read_bytes += bytes;
    }

    /* The path's end kept when it is longer than the row: a file's name is
     * what a person recognises, and it is at the end. */
    path = path ? path : "";
    n = strlen(path);

    if (n >= DEVICE_LAST_MAX) {
        path += n - (DEVICE_LAST_MAX - 1u);
        n = DEVICE_LAST_MAX - 1u;
    }

    memcpy(r->last, path, n);
    r->last[n] = '\0';
}

unsigned progcount_page(const struct progcount *t, unsigned from,
                        struct disk_program *out, unsigned most, uint32_t *more)
{
    unsigned n = 0;

    while (n < most && from + n < t->count) {
        out[n] = t->rows[from + n];
        n++;
    }

    *more = (from + n < t->count) ? 1u : 0u;
    return n;
}
