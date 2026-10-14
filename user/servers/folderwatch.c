/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Folders that tell (`folderwatch.h`, `docs/pulses.md` P4).
 *
 * **Paths are compared as the server sees them**, with any slashes at the
 * ends left out: "Home/Documents/", "/Home/Documents" and "Home/Documents"
 * are one folder, and a change to "Home/Documents/a.txt" is a change in it.
 * Both a watch and the change it waits for arrive through the same server's
 * door, in the same terms, so nothing here has to know about mounts.
 *
 * **The list grows** (`CLAUDE.md`: no compiled-in limits): a window per
 * folder it shows, an automation per folder it watches - a handful, but no
 * number of them is refused while there is memory.
 */

#include <string.h>

#include "folderwatch.h"

/* `path` without slashes at either end, into `out`; its length. */
static size_t trimmed(const char *path, char out[FOLDERWATCH_PATH_MAX])
{
    size_t start = 0, end = strlen(path);

    while (start < end && path[start] == '/') {
        start++;
    }

    while (end > start && path[end - 1] == '/') {
        end--;
    }

    if (end - start >= FOLDERWATCH_PATH_MAX) {
        end = start + FOLDERWATCH_PATH_MAX - 1;
    }

    memcpy(out, path + start, end - start);
    out[end - start] = '\0';
    return end - start;
}

/* The folder `path` is in, trimmed: "" for one at the top. */
static void parent_of(const char *path, char out[FOLDERWATCH_PATH_MAX])
{
    size_t n = trimmed(path, out);

    while (n > 0 && out[n - 1] != '/') {
        n--;
    }

    while (n > 0 && out[n - 1] == '/') {
        n--;
    }

    out[n] = '\0';
}

bool folderwatch_add(struct folderwatch *fw, const char *dir, long cap,
                     uint32_t code, uint32_t owner)
{
    char folder[FOLDERWATCH_PATH_MAX];
    size_t i;

    trimmed(dir, folder);

    for (i = 0; i < fw->used; i++) {
        struct folderwatch_entry *e = &fw->list[i];

        if (e->owner == owner && e->code == code) {
            if (e->cap != cap && fw->drop) {
                fw->drop(e->cap);
            }

            memcpy(e->dir, folder, sizeof(folder));
            e->cap = cap;
            return true;
        }
    }

    if (fw->used == fw->room) {
        size_t room = fw->room ? fw->room * 2 : 8;
        void *more = fw->grow ? fw->grow(fw->list, room * sizeof(*fw->list)) : NULL;

        if (more == NULL) {
            return false;
        }

        fw->list = more;
        fw->room = room;
    }

    memcpy(fw->list[fw->used].dir, folder, sizeof(folder));
    fw->list[fw->used].cap = cap;
    fw->list[fw->used].code = code;
    fw->list[fw->used].owner = owner;
    fw->used++;
    return true;
}

static void forget(struct folderwatch *fw, size_t i)
{
    if (fw->drop) {
        fw->drop(fw->list[i].cap);
    }

    fw->list[i] = fw->list[--fw->used];
}

void folderwatch_remove(struct folderwatch *fw, uint32_t code, uint32_t owner)
{
    size_t i = 0;

    while (i < fw->used) {
        if (fw->list[i].owner == owner && fw->list[i].code == code) {
            forget(fw, i);
        } else {
            i++;
        }
    }
}

size_t folderwatch_changed(struct folderwatch *fw, const char *path)
{
    char folder[FOLDERWATCH_PATH_MAX];
    size_t i = 0, told = 0;

    if (fw->used == 0) {
        return 0;
    }

    parent_of(path, folder);
    fw->changes++;

    while (i < fw->used) {
        struct folderwatch_entry *e = &fw->list[i];

        if (strcmp(e->dir, folder) != 0) {
            i++;
            continue;
        }

        /* A subscriber that is gone - its endpoint destroyed - is let go;
         * a full ring is its business, and the next change tells it again. */
        if (fw->pulse && fw->pulse(e->cap, e->code, fw->changes) == -1) {
            forget(fw, i);
            continue;
        }

        told++;
        i++;
    }

    return told;
}
