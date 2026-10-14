/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Folders that tell** (`docs/pulses.md` P4): who watches which folder,
 * and a pulse to each of them when a name in it is added, removed, renamed
 * or written. One door for both filesystem servers - `diskfs` (`/Home`) and
 * `ramfs` (`/Temporary`) - rather than a copy in each.
 *
 * A watch is a folder, the capability a subscriber passed with its request
 * (the window manager's endpoint, for a window), the code to pulse it with,
 * and who asked - so a watch is taken back by whoever made it. The pulse's
 * value is a counter of changes, which says nothing but that something did.
 *
 * The kernel's half is passed in (`pulse`, `drop`) so this is tested on the
 * Mac without one (`tools/test_folderwatch.c`).
 */

#ifndef KOSMOS_FOLDERWATCH_H
#define KOSMOS_FOLDERWATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FOLDERWATCH_PATH_MAX 256

struct folderwatch_entry {
    char     dir[FOLDERWATCH_PATH_MAX];
    long     cap;
    uint32_t code;
    uint32_t owner;
};

struct folderwatch {
    struct folderwatch_entry *list;
    size_t used, room;
    uint64_t changes;

    /* 0, or the kernel's IPC result - -1 is a capability that names
     * nothing, and that watch is let go. */
    long (*pulse)(long cap, uint32_t code, uint64_t value);
    void (*drop)(long cap);
    void *(*grow)(void *old, size_t bytes);     /* realloc */
};

/* Watched: `dir` by `owner`, told by pulsing `cap` with `code`. The same
 * owner and code again moves that watch to `dir`, and lets the old
 * capability go. False when there is no memory for it. */
bool folderwatch_add(struct folderwatch *fw, const char *dir, long cap,
                     uint32_t code, uint32_t owner);

/* `owner`'s watch with `code` taken back, its capability let go. */
void folderwatch_remove(struct folderwatch *fw, uint32_t code, uint32_t owner);

/* Something at `path` changed: every watcher of the folder it is in is
 * pulsed. Returns how many were. */
size_t folderwatch_changed(struct folderwatch *fw, const char *path);

#endif
