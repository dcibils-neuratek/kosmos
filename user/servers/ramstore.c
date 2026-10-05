/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /Temporary's store: files, attributes and live queries, in memory.
 *
 * What `ramfs.c` serves - and `/Home` on a machine with no disk - kept here
 * apart from the receiving and replying, so the host can hold it to what it
 * does (`tools/test_ramstore.c`). The server is the seventh to move to C
 * and the last; its history is in `ramfs.c`.
 *
 * **A flat table of paths, not a tree.** The Lua version kept both: a tree
 * of `children` for listing and a `nodes` map keyed by path for the index.
 * Two representations of one fact, kept in step by hand, and `write` had to
 * remember to touch both. Here there is one array and a listing is a scan
 * for paths one level below the prefix - O(n) where the tree was
 * O(children). A directory is a path with no value rather than a node of a
 * different shape.
 *
 * **It grows, to a ceiling the machine sets** (5 October 2026, step 4 after
 * 0.11). It was 128 entries of 16 KB each, compiled in: a PDF of five faces
 * did not fit (`testing.md` 18.382), a song never could, and `/Home` on a
 * machine with no disk is this store. The pool was fixed on the kernel's
 * argument - a full store an error at a known limit rather than a failure
 * at an unknown one - and that argument is kept by the ceiling rather than
 * by the sizes: an entry, its value and a parked watch each come from the
 * process's own heap as they are needed and go back when they are deleted,
 * and everything held is counted against `ceiling`, which `ramfs.c` sets to
 * half of the machine's memory. Past it a write is refused and changes
 * nothing it was given - the same clean refusal, at a limit the machine
 * decides rather than one compiled in.
 * `CLAUDE.md`'s pools grow and are never freed because they are the
 * kernel's; this is a process, and a file deleted gives its bytes back.
 *
 * **The heap and not a mapping per value**, because the kernel never reuses
 * an address it mapped (`kernel/process.h`): a file rewritten every second
 * with fresh pages would walk this process through its four gigabytes of
 * addresses in a few days. `malloc` keeps what is freed and hands it out
 * again, so the addresses used follow the most ever held, not the number of
 * writes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ramstore.h"

/*
 * A value at least this large gives its room back when it is written again
 * from the start, rather than keeping it for the next writer: a status file
 * rewritten every second keeps its room and is never copied, and a song
 * replaced by a line of text does not hold a song's worth of memory.
 */
#define ROOM_KEPT  (1024u * 1024u)

struct node {
    bool     directory;
    char     path[RAM_PATH_MAX];
    char    *value;               /* `room` bytes from the heap, or NULL */
    uint32_t room;
    uint32_t length;
    uint32_t packed;              /* text, or a serialised Lua value */
    struct ram_attr attrs[RAM_ATTRS_MAX];
    unsigned nattrs;
};

/*
 * The store: every entry, in no order - a listing and a query sort what
 * they hand back. Each entry is its own allocation, so a pointer to one
 * stays good while the array of them grows; a deleted one is replaced by
 * the last.
 */
static struct node **nodes;
static unsigned nodes_used, nodes_room;

/* What is held against the ceiling, in bytes. */
static size_t held, ceiling;

/*
 * Somebody waiting for a query's answer to change.
 *
 * `who` is a sender the kernel gave us and we have not replied to, which is
 * the same trick the console plays with `read`: the caller is parked in
 * `call` and nothing here is blocked on it.
 */
struct watcher {
    uint64_t who;
    /* Where it was asked, so re-evaluating it later scopes the same way the
     * first evaluation did. Without it a watch on one subtree would start
     * firing for changes in another. */
    char     under[RAM_PATH_MAX];
    struct ram_attr where[RAM_ATTRS_MAX];
    unsigned nwhere;
    char     last[RAM_ENTRIES_MAX][RAM_PATH_MAX];
    unsigned nlast;
};

/* Parked watches, as the nodes are: each its own allocation, a gone one
 * replaced by the last. */
static struct watcher **watchers;
static unsigned watchers_used, watchers_room;

/*------------------------------------------------------------------------
 * Small string work, spelled out because there is no libc worth the name.
 *----------------------------------------------------------------------*/

static void copy_into(char *dst, size_t cap, const char *src, size_t n)
{
    if (n >= cap) {
        n = cap - 1;
    }

    memcpy(dst, src, n);
    dst[n] = '\0';
}

/*
 * One canonical spelling of a path: a leading slash, no trailing one, "/"
 * for the root.
 *
 * Callers send "/a/b", "a/b" and "/a/b/" for the same thing, and the leading
 * slash has to survive rather than be stripped - the namespace joins a
 * mount's prefix onto whatever comes back, so a path returned as "a" becomes
 * "/dataa" instead of "/Temporary/a". Found by a query, which is the only
 * operation that hands whole paths back.
 */
static void normalise(char *dst, const char *src)
{
    size_t n = strlen(src);
    size_t start = 0;
    size_t len;

    while (start < n && src[start] == '/') {
        start++;
    }

    while (n > start && src[n - 1] == '/') {
        n--;
    }

    len = n - start;

    if (len > RAM_PATH_MAX - 2) {
        len = RAM_PATH_MAX - 2;
    }

    dst[0] = '/';
    memcpy(dst + 1, src + start, len);
    dst[1 + len] = '\0';
}

static bool is_root(const char *path)
{
    return path[0] == '/' && path[1] == '\0';
}

/*
 * **Whatever its case** (`roadmap.md` 6s): `/Notes.txt` finds `/notes.txt`,
 * and the store cannot hold both. A path keeps the spelling it was made
 * with, directories included (`spell_parents`), so what a listing or a
 * query says is what was typed when the file was made.
 */
static struct node *find(const char *path)
{
    unsigned i;

    for (i = 0; i < nodes_used; i++) {
        if (strcasecmp(nodes[i]->path, path) == 0) {
            return nodes[i];
        }
    }

    return NULL;
}

/*
 * Is `path` a name directly inside `dir`?
 *
 * The whole of listing, and the reason a tree was not needed. `dir` is "" at
 * the root, where every path with no slash in it is a child.
 */
static const char *child_of(const char *dir, const char *path)
{
    size_t n = strlen(dir);
    const char *rest;

    if (is_root(dir)) {
        rest = path + 1;                  /* "/a" is a child of "/" */
    } else {
        if (strncasecmp(path, dir, n) != 0 || path[n] != '/') {
            return NULL;
        }

        rest = path + n + 1;
    }

    if (*rest == '\0' || strchr(rest, '/') != NULL) {
        return NULL;                  /* deeper than one level, or itself */
    }

    return rest;
}

/*
 * Is there anything inside `dir`?
 *
 * One scan, and it stops at the first hit. A flat table makes this the same
 * shape as listing - `child_of` is the whole of both - where a tree would
 * have asked a node how many children it had.
 */
static bool has_children(const char *dir)
{
    unsigned i;

    for (i = 0; i < nodes_used; i++) {
        if (child_of(dir, nodes[i]->path) != NULL) {
            return true;
        }
    }

    return false;
}

/*
 * The directories a path implies, made real.
 *
 * Writing `/a/b/c` means `/a` and `/a/b` are directories, and the Lua
 * version got that for free by walking a tree and creating nodes as it went.
 * With a flat table it has to be said out loud.
 */
static struct node *claim(const char *path);

/*
 * `path`'s directories as they are spelled here, its last name as given.
 *
 * A new file's directories are found whatever the case they were typed in,
 * and a flat table stores each node's whole path - so without this, a file
 * made as `/NOTES/x` in the directory `/notes` would keep `/NOTES/x`, and a
 * query would hand back a path whose directory nobody made. Folding keeps
 * the length of an ASCII name, which is what lets the spelling be copied
 * over in place.
 */
static void spell_parents(char *path)
{
    size_t i;

    for (i = 1; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            char dir[RAM_PATH_MAX];
            struct node *d;

            copy_into(dir, sizeof(dir), path, i);
            d = find(dir);

            if (d != NULL && strlen(d->path) == i) {
                memcpy(path, d->path, i);
            }
        }
    }
}

static void ensure_parents(const char *path)
{
    char dir[RAM_PATH_MAX];
    size_t i;

    /* From 1, not 0: every path starts with a slash and the empty string
     * above it is not a directory anyone can name. */
    for (i = 1; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            copy_into(dir, sizeof(dir), path, i);

            if (find(dir) == NULL) {
                struct node *d = claim(dir);

                if (d != NULL) {
                    d->directory = true;
                }
            }
        }
    }
}

/*
 * Room for `more` bytes against the ceiling, which is the whole of the
 * store's limit: everything that takes memory asks here first.
 */
static bool room_for(size_t more)
{
    return more <= ceiling && held <= ceiling - more;
}

/*
 * An array of pointers with room for one more, doubled when it is full:
 * the array, moved or not, or NULL with the old one untouched. The
 * pointers are a word an entry and are not counted against the ceiling -
 * what they point at is.
 */
static void *one_more(void *array, unsigned used, unsigned *room, size_t each)
{
    void *grown;
    unsigned want;

    if (used < *room) {
        return array;
    }

    want = (*room == 0) ? 64u : *room * 2u;

    if (want <= *room) {
        return NULL;                  /* doubled past what an unsigned holds */
    }

    grown = realloc(array, (size_t)want * each);

    if (grown != NULL) {
        *room = want;
    }

    return grown;
}

static struct node *claim(const char *path)
{
    struct node *n = find(path);
    struct node **more;

    if (n != NULL) {
        return n;
    }

    if (!room_for(sizeof(*n))) {
        return NULL;
    }

    more = one_more(nodes, nodes_used, &nodes_room, sizeof(*nodes));

    if (more == NULL) {
        return NULL;
    }

    nodes = more;
    n = calloc(1, sizeof(*n));

    if (n == NULL) {
        return NULL;
    }

    held += sizeof(*n);
    copy_into(n->path, RAM_PATH_MAX, path, strlen(path));
    spell_parents(n->path);
    nodes[nodes_used++] = n;
    return n;
}

/* Its value's room given back: the node keeps its name and its attributes. */
static void let_go_of_value(struct node *n)
{
    free(n->value);
    held -= n->room;
    n->value = NULL;
    n->room = 0;
}

/* An entry gone, and its memory with it. */
static void forget(struct node *n)
{
    unsigned i;

    for (i = 0; i < nodes_used; i++) {
        if (nodes[i] == n) {
            nodes[i] = nodes[--nodes_used];
            break;
        }
    }

    let_go_of_value(n);
    held -= sizeof(*n);
    free(n);
}

/*
 * Room in a value for `want` bytes, kept if it is there and grown if it is
 * not - doubling, so a file written a message at a time is copied a
 * handful of times rather than once a message, and exactly to `want` when
 * doubling would pass the ceiling and the exact size would not. False, and
 * nothing changed, when neither fits.
 */
static bool make_room(struct node *n, uint64_t want)
{
    uint64_t room;
    char *grown;

    if (want <= n->room) {
        return true;
    }

    if (want > UINT32_MAX) {
        return false;
    }

    room = (n->room == 0) ? 256u : n->room;

    while (room < want) {
        room *= 2u;
    }

    if (room > UINT32_MAX || !room_for((size_t)(room - n->room))) {
        room = want;

        if (!room_for((size_t)(room - n->room))) {
            return false;
        }
    }

    grown = realloc(n->value, (size_t)room);

    if (grown == NULL) {
        return false;
    }

    held += (size_t)(room - n->room);
    n->value = grown;
    n->room = (uint32_t)room;
    return true;
}

/*------------------------------------------------------------------------
 * Attributes.
 *----------------------------------------------------------------------*/

static struct ram_attr *attr_of(struct node *n, const char *name)
{
    unsigned i;

    for (i = 0; i < n->nattrs; i++) {
        if (strcmp(n->attrs[i].name, name) == 0) {
            return &n->attrs[i];
        }
    }

    return NULL;
}

static bool set_attr(struct node *n, const struct ram_attr *a)
{
    struct ram_attr *slot = attr_of(n, a->name);

    if (slot == NULL) {
        if (n->nattrs >= RAM_ATTRS_MAX) {
            return false;
        }

        slot = &n->attrs[n->nattrs++];
    }

    *slot = *a;
    return true;
}

/* An unsigned number as text, for the `size` attribute a write maintains. */
static void number_attr(struct ram_attr *a, const char *name, uint32_t v)
{
    char buf[16];
    unsigned i = sizeof(buf);

    buf[--i] = '\0';

    if (v == 0) {
        buf[--i] = '0';
    }

    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    }

    memset(a, 0, sizeof(*a));
    a->kind = RAM_ATTR_NUMBER;
    copy_into(a->name, RAM_NAME_MAX, name, strlen(name));
    copy_into(a->value, RAM_VALUE_MAX, buf + i, strlen(buf + i));
}

/*
 * Does this node match every term?
 *
 * Compared as text whatever the kinds say, which is what the Lua version did
 * - it wrote `tostring(node.attrs[name]) ~= tostring(value)` - and keeping
 * that means a query for `size = 4` still finds what `size = "4"` set.
 */
static bool matches(struct node *n, const struct ram_attr *where,
                    unsigned nwhere)
{
    unsigned i;

    for (i = 0; i < nwhere; i++) {
        struct ram_attr *have = attr_of(n, where[i].name);

        if (have == NULL || strcmp(have->value, where[i].value) != 0) {
            return false;
        }
    }

    return true;
}

/*
 * Every path matching `where`, in path order, into a page of a reply.
 *
 * Sorted so a listing is reproducible - the Lua version sorted for the same
 * reason, and a query that returned its answers in table order returned them
 * differently between runs of the same program.
 */
static unsigned evaluate(const char *under,
                         const struct ram_attr *where, unsigned nwhere,
                         char out[][RAM_PATH_MAX], unsigned cap,
                         unsigned skip, bool *more)
{
    unsigned found = 0, taken = 0;
    const char *last = NULL;

    /*
     * Under `under`, and not the whole store.
     *
     * A query is asked at a path and the honest reading of that is "what is
     * below here". The root asks about everything, which is what a query at
     * the mount point does. The same fix as the disk's, which is where the
     * bug actually bit: one filesystem mounted at three places was answering
     * a question about `/Home` with what is under `/system`.
     */
    size_t under_len = (under != NULL) ? strlen(under) : 0;
    bool whole = (under_len == 0) || (under_len == 1 && under[0] == '/');

    *more = false;

    /* A selection sort over the store rather than an array to sort: the
     * answer is a page of four, so this walks the store once per result
     * rather than copying and sorting the whole of it. */
    for (;;) {
        const char *best = NULL;
        unsigned i;

        for (i = 0; i < nodes_used; i++) {
            struct node *n = nodes[i];

            if (n->directory) {
                continue;
            }

            if (nwhere > 0 && !matches(n, where, nwhere)) {
                continue;
            }

            if (!whole
                && (strncasecmp(n->path, under, under_len) != 0
                    || (n->path[under_len] != '\0'
                        && n->path[under_len] != '/'))) {
                continue;
            }

            if (last != NULL && strcmp(n->path, last) <= 0) {
                continue;
            }

            if (best == NULL || strcmp(n->path, best) < 0) {
                best = n->path;
            }
        }

        if (best == NULL) {
            return taken;
        }

        last = best;

        if (found++ < skip) {
            continue;
        }

        if (taken >= cap) {
            *more = true;
            return taken;
        }

        copy_into(out[taken], RAM_PATH_MAX, best, strlen(best));
        taken++;
    }
}

/*------------------------------------------------------------------------
 * Watchers.
 *----------------------------------------------------------------------*/

static bool same_paths(char a[][RAM_PATH_MAX], unsigned na,
                       char b[][RAM_PATH_MAX], unsigned nb)
{
    unsigned i;

    if (na != nb) {
        return false;
    }

    for (i = 0; i < na; i++) {
        if (strcmp(a[i], b[i]) != 0) {
            return false;
        }
    }

    return true;
}

static void fail(uint64_t to, uint32_t code)
{
    struct ram_reply rep;

    memset(&rep, 0, sizeof(rep));
    rep.error = code;
    ram_reply(to, &rep);
}

/* A parked watch gone - answered, or the store emptied - and its memory
 * with it. */
static void unpark(unsigned i)
{
    struct watcher *w = watchers[i];

    watchers[i] = watchers[--watchers_used];
    held -= sizeof(*w);
    free(w);
}

/*
 * Anything whose answer changed gets it now.
 *
 * Called after every write and every setattr, which is the only way the set
 * of matching paths can move. A watcher whose answer is unchanged stays
 * parked - it asked to be told when something happened, not when something
 * was written.
 */
static void notify(void)
{
    unsigned i = 0;

    while (i < watchers_used) {
        struct watcher *w = watchers[i];
        struct ram_reply rep;
        bool more;

        memset(&rep, 0, sizeof(rep));
        rep.count = evaluate(w->under, w->where, w->nwhere, rep.u.entries,
                             RAM_ENTRIES_MAX, 0, &more);
        rep.more = more ? 1u : 0u;

        if (same_paths(rep.u.entries, rep.count, w->last, w->nlast)) {
            i++;
            continue;
        }

        /* Answered, and gone: the last takes its place, so `i` stays. */
        ram_reply(w->who, &rep);
        unpark(i);
    }
}

/*------------------------------------------------------------------------
 * The operations.
 *----------------------------------------------------------------------*/

void ram_answer(const void *data, size_t length, uint64_t sender)
{
    struct ram_request req;
    struct ram_reply rep;
    char path[RAM_PATH_MAX];
    struct node *n;
    unsigned i;

    if (length < sizeof(req)) {
        fail(sender, RAM_ERR_BAD_OP);
        return;
    }

    memcpy(&req, data, sizeof(req));
    memset(&rep, 0, sizeof(rep));

    /* Whatever arrived, terminated. `path` is 256 bytes from another
     * process and nothing promises there is a zero in it. */
    req.path[RAM_PATH_MAX - 1] = '\0';
    normalise(path, req.path);

    if (req.count > RAM_ATTRS_MAX) {
        req.count = RAM_ATTRS_MAX;
    }

    for (i = 0; i < RAM_ATTRS_MAX; i++) {
        req.attrs[i].name[RAM_NAME_MAX - 1] = '\0';
        req.attrs[i].value[RAM_VALUE_MAX - 1] = '\0';
    }

    switch (req.op) {
    case RAM_OP_LIST: {
        unsigned found = 0, taken = 0;
        const char *last = NULL;
        bool is_dir = is_root(path);

        n = find(path);

        if (!is_dir) {
            if (n == NULL) {
                fail(sender, RAM_ERR_NO_PATH);
                return;
            }

            if (!n->directory) {
                fail(sender, RAM_ERR_NOT_DIR);
                return;
            }
        }

        /* The same selection walk `evaluate` uses, over children rather
         * than over matches. */
        for (;;) {
            const char *best = NULL;
            unsigned k;

            for (k = 0; k < nodes_used; k++) {
                const char *name = child_of(path, nodes[k]->path);

                if (name == NULL) {
                    continue;
                }

                if (last != NULL && strcmp(name, last) <= 0) {
                    continue;
                }

                if (best == NULL || strcmp(name, best) < 0) {
                    best = name;
                }
            }

            if (best == NULL) {
                break;
            }

            last = best;

            if (found++ < req.offset) {
                continue;
            }

            if (taken >= RAM_ENTRIES_MAX) {
                rep.more = 1u;
                break;
            }

            copy_into(rep.u.entries[taken], RAM_PATH_MAX, best, strlen(best));
            taken++;
        }

        rep.count = taken;
        break;
    }

    case RAM_OP_READ: {
        uint32_t at = req.offset;
        uint32_t take;

        n = find(path);

        if (n == NULL) {
            fail(sender, RAM_ERR_NO_PATH);
            return;
        }

        if (n->directory) {
            fail(sender, RAM_ERR_NOT_READABLE);
            return;
        }

        if (at > n->length) {
            at = n->length;
        }

        take = n->length - at;

        if (take > RAM_DATA_MAX) {
            take = RAM_DATA_MAX;
            rep.more = 1u;
        }

        if (take > 0) {
            memcpy(rep.u.data, n->value + at, take);
        }

        rep.length = take;
        rep.packed = n->packed;
        break;
    }

    case RAM_OP_WRITE: {
        struct ram_attr size;
        uint64_t end;
        bool made;

        if (is_root(path)) {
            fail(sender, RAM_ERR_NO_PATH);   /* the root is not a file */
            return;
        }

        if (req.length > RAM_DATA_MAX) {
            req.length = RAM_DATA_MAX;
        }

        ensure_parents(path);
        made = (find(path) == NULL);
        n = claim(path);

        if (n == NULL) {
            fail(sender, RAM_ERR_FULL);
            return;
        }

        /*
         * An offset makes a write a stream.
         *
         * The Lua version replaced the value every time, so a file larger
         * than a message could not be written at all - and the namespace
         * split long writes without anything on this side putting them back
         * together. Writing at 0 truncates, which is what `fs.write` means.
         */
        if (req.offset == 0) {
            n->length = 0;
            n->packed = req.packed;

            if (n->room >= ROOM_KEPT) {
                let_go_of_value(n);
            }
        }

        /* In 64 bits: an offset and a length from another process add up to
         * whatever they like. */
        end = (uint64_t)req.offset + req.length;

        /* Refused past the ceiling, and a file this write would have made
         * is not left behind empty. */
        if (!make_room(n, end)) {
            if (made) {
                forget(n);
            }

            fail(sender, RAM_ERR_FULL);
            return;
        }

        /* Past the end is a gap, and a gap reads as zeros - not as what the
         * heap held before, which may be somebody else's file. */
        if (req.offset > n->length) {
            memset(n->value + n->length, 0, req.offset - n->length);
        }

        if (req.length > 0) {
            memcpy(n->value + req.offset, req.u.data, req.length);
        }

        if (end > n->length) {
            n->length = (uint32_t)end;
        }

        n->directory = false;

        /* Only text gets a size, which is what the Lua version did: it set
         * the attribute when the value was a string and left it alone
         * otherwise, so `ls` shows a length for a file and nothing for a
         * stored table. */
        if (n->packed == RAM_RAW) {
            number_attr(&size, "size", n->length);
            (void)set_attr(n, &size);
        }

        notify();
        break;
    }

    case RAM_OP_GETATTR:
        n = find(path);

        if (n == NULL) {
            fail(sender, RAM_ERR_NO_PATH);
            return;
        }

        for (i = 0; i < n->nattrs && i < RAM_ATTRS_MAX; i++) {
            rep.u.attrs[i] = n->attrs[i];
        }

        rep.count = i;

        /* A directory says so, the way the Lua tree did by holding
         * `children`. Added rather than stored, so the fact has one home. */
        if (n->directory && rep.count < RAM_ATTRS_MAX) {
            struct ram_attr *a = &rep.u.attrs[rep.count++];

            memset(a, 0, sizeof(*a));
            a->kind = RAM_ATTR_TEXT;
            copy_into(a->name, RAM_NAME_MAX, "kind", 4);
            copy_into(a->value, RAM_VALUE_MAX, "directory", 9);
        }

        break;

    case RAM_OP_SETATTR:
        ensure_parents(path);
        n = claim(path);

        if (n == NULL) {
            fail(sender, RAM_ERR_FULL);
            return;
        }

        for (i = 0; i < req.count; i++) {
            if (!set_attr(n, &req.attrs[i])) {
                fail(sender, RAM_ERR_TOO_MANY);
                return;
            }
        }

        notify();
        break;

    case RAM_OP_QUERY: {
        bool more;

        rep.count = evaluate(path, req.attrs, req.count, rep.u.entries,
                             RAM_ENTRIES_MAX, req.offset, &more);
        rep.more = more ? 1u : 0u;
        break;
    }

    case RAM_OP_WATCH: {
        struct watcher *w = NULL;
        bool more;
        unsigned known = req.count;

        /*
         * `count` means the query's terms everywhere else and the caller's
         * known paths here, which is the one place this protocol is not
         * uniform. `offset` carries the number of terms instead - a watch
         * never pages, so the field was free.
         */
        unsigned nwhere = req.offset;

        if (nwhere > RAM_ATTRS_MAX) {
            nwhere = RAM_ATTRS_MAX;
        }

        if (known > RAM_ENTRIES_MAX) {
            known = RAM_ENTRIES_MAX;
        }

        for (i = 0; i < known; i++) {
            req.u.known[i][RAM_PATH_MAX - 1] = '\0';
        }

        rep.count = evaluate(path, req.attrs, nwhere, rep.u.entries,
                             RAM_ENTRIES_MAX, 0, &more);
        rep.more = more ? 1u : 0u;

        /* Already different from what the caller has: answer now. */
        if (!same_paths(rep.u.entries, rep.count, req.u.known, known)) {
            break;
        }

        if (room_for(sizeof(*w))) {
            struct watcher **more = one_more(watchers, watchers_used,
                                             &watchers_room, sizeof(*watchers));

            if (more != NULL) {
                watchers = more;
                w = calloc(1, sizeof(*w));
            }
        }

        if (w == NULL) {
            fail(sender, RAM_ERR_FULL);
            return;
        }

        held += sizeof(*w);
        watchers[watchers_used++] = w;
        w->who = sender;
        w->nwhere = nwhere;
        copy_into(w->under, RAM_PATH_MAX, path, strlen(path));

        for (i = 0; i < nwhere; i++) {
            w->where[i] = req.attrs[i];
        }

        w->nlast = rep.count;

        for (i = 0; i < rep.count; i++) {
            memcpy(w->last[i], rep.u.entries[i], RAM_PATH_MAX);
        }

        /* No reply: the caller is parked until `notify` finds a change. */
        return;
    }

    case RAM_OP_DELETE:
        if (is_root(path)) {
            fail(sender, RAM_ERR_NO_PATH);   /* the root is not a file */
            return;
        }

        n = find(path);

        if (n == NULL) {
            fail(sender, RAM_ERR_NO_PATH);
            return;
        }

        /*
         * A directory goes only when it is empty, which is the disk's rule
         * and is worth being the same here: it is the one thing between a
         * mistyped path and a subtree. `rm -r` is the caller agreeing to do
         * the walk, and every step of it is a delete this would allow.
         */
        if (n->directory && has_children(path)) {
            fail(sender, RAM_ERR_NOT_EMPTY);
            return;
        }

        forget(n);
        notify();
        break;

    case RAM_OP_MKDIR:
        if (is_root(path)) {
            fail(sender, RAM_ERR_EXISTS);    /* the root is always there */
            return;
        }

        if (find(path) != NULL) {
            fail(sender, RAM_ERR_EXISTS);
            return;
        }

        ensure_parents(path);
        n = claim(path);

        if (n == NULL) {
            fail(sender, RAM_ERR_FULL);
            return;
        }

        n->directory = true;
        notify();
        break;

    case RAM_OP_RENAME: {
        char to[RAM_PATH_MAX];
        size_t from_len;

        normalise(to, req.u.data);
        spell_parents(to);

        if (is_root(path) || is_root(to)) {
            fail(sender, RAM_ERR_NO_PATH);
            return;
        }

        n = find(path);

        if (n == NULL) {
            fail(sender, RAM_ERR_NO_PATH);
            return;
        }

        /* Its own path in another case is not taken: `/notes` to
         * `/Notes` is how a spelling is changed. */
        if (find(to) != NULL && find(to) != n) {
            fail(sender, RAM_ERR_EXISTS);
            return;
        }

        from_len = strlen(path);

        /*
         * A directory into itself, refused here because only this side can
         * see it: `/a` to `/a/b` would either loop or orphan everything
         * under it, and the string test is exact because both are absolute
         * paths through the same flat table.
         */
        if (strncasecmp(to, path, from_len) == 0 && to[from_len] == '/') {
            fail(sender, RAM_ERR_NO_PATH);
            return;
        }

        /*
         * **Everything underneath moves too, and that is the price of a flat
         * table.** A tree renames one node and the children come along
         * because they hang off it; here a child is a *string* that starts
         * with the parent's, so each one has to be rewritten. `/a/b` under a
         * rename of `/a` to `/c` becomes `/c/b` by replacing the prefix.
         */
        for (i = 0; i < nodes_used; i++) {
            struct node *child = nodes[i];
            char rebuilt[RAM_PATH_MAX];

            if (strncasecmp(child->path, path, from_len) != 0
                || child->path[from_len] != '/') {
                continue;
            }

            copy_into(rebuilt, sizeof(rebuilt), to, strlen(to));
            copy_into(rebuilt + strlen(rebuilt),
                      sizeof(rebuilt) - strlen(rebuilt),
                      child->path + from_len,
                      strlen(child->path + from_len));

            copy_into(child->path, RAM_PATH_MAX, rebuilt, strlen(rebuilt));
        }

        copy_into(n->path, RAM_PATH_MAX, to, strlen(to));
        ensure_parents(n->path);
        notify();
        break;
    }

    default:
        fail(sender, RAM_ERR_BAD_OP);
        return;
    }

    ram_reply(sender, &rep);
}

void ram_store_init(size_t most)
{
    while (watchers_used > 0) {
        unpark(watchers_used - 1);
    }

    while (nodes_used > 0) {
        forget(nodes[nodes_used - 1]);
    }

    held = 0;
    ceiling = most;
}

size_t ram_store_held(void)
{
    return held;
}

unsigned ram_store_entries(void)
{
    return nodes_used;
}

unsigned ram_store_watches(void)
{
    return watchers_used;
}
