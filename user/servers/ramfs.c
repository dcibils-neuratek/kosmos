/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /data: files, attributes and live queries, in memory.
 *
 * The seventh server to move and the last one that will. It is also the one
 * whose conversion cost something rather than only buying: ramfs was what
 * `ROLE_RELOAD` reloaded and what `help("demos")` let you *watch* being
 * reloaded, and with it in C there is no server left in the system whose
 * code can be replaced while it runs. That was decided rather than
 * discovered - see `docs/design.md` - and the honest description is that hot
 * reload is a feature this system had and removed, not one that lost a
 * tiebreaker.
 *
 * **A flat table of paths, not a tree.** The Lua version kept both: a tree
 * of `children` for listing and a `nodes` map keyed by path for the index.
 * Two representations of one fact, kept in step by hand, and `write` had to
 * remember to touch both. Here there is one array and a listing is a scan
 * for paths one level below the prefix - O(n) where the tree was O(children),
 * and n is 128. A directory is a path with no value rather than a node of a
 * different shape.
 *
 * **No malloc.** Userland has one, and this does not use it: a fixed pool
 * with a fixed value buffer per node means the filesystem's memory is
 * decided at compile time and a full disk is an error at a known limit
 * rather than a failure at an unknown one. That is the kernel's rule, and
 * it is a good rule here for the same reason.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "ramproto.h"

#define NODES        128u
/*
 * The biggest thing /data will hold.
 *
 * 4096 was the first guess and it was too small by inspection rather than by
 * failure: a replicant publishes a table holding its own source, and
 * `clock-replicant.lua` alone is 1824 bytes before the serialiser adds its
 * framing. 16 KB gives that room to grow without making the pool - 128 nodes
 * - large enough to matter against a 512 MB machine.
 */
#define VALUE_MAX  16384u
#define WATCHERS      16u

struct node {
    bool     used;
    bool     directory;
    char     path[RAM_PATH_MAX];
    char     value[VALUE_MAX];
    uint32_t length;
    uint32_t packed;              /* text, or a serialised Lua value */
    struct ram_attr attrs[RAM_ATTRS_MAX];
    unsigned nattrs;
};

static struct node nodes[NODES];

/*
 * Somebody waiting for a query's answer to change.
 *
 * `who` is a sender the kernel gave us and we have not replied to, which is
 * the same trick the console plays with `read`: the caller is parked in
 * `call` and nothing here is blocked on it.
 */
struct watcher {
    bool     used;
    uint64_t who;
    struct ram_attr where[RAM_ATTRS_MAX];
    unsigned nwhere;
    char     last[RAM_ENTRIES_MAX][RAM_PATH_MAX];
    unsigned nlast;
};

static struct watcher watchers[WATCHERS];

static uint32_t writes;

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
 * "/dataa" instead of "/data/a". Found by a query, which is the only
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

static struct node *find(const char *path)
{
    unsigned i;

    for (i = 0; i < NODES; i++) {
        if (nodes[i].used && strcmp(nodes[i].path, path) == 0) {
            return &nodes[i];
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
        if (strncmp(path, dir, n) != 0 || path[n] != '/') {
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
 * The directories a path implies, made real.
 *
 * Writing `/a/b/c` means `/a` and `/a/b` are directories, and the Lua
 * version got that for free by walking a tree and creating nodes as it went.
 * With a flat table it has to be said out loud.
 */
static struct node *claim(const char *path);

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

static struct node *claim(const char *path)
{
    struct node *n = find(path);
    unsigned i;

    if (n != NULL) {
        return n;
    }

    for (i = 0; i < NODES; i++) {
        if (!nodes[i].used) {
            memset(&nodes[i], 0, sizeof(nodes[i]));
            nodes[i].used = true;
            copy_into(nodes[i].path, RAM_PATH_MAX, path, strlen(path));
            return &nodes[i];
        }
    }

    return NULL;
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
static unsigned evaluate(const struct ram_attr *where, unsigned nwhere,
                         char out[][RAM_PATH_MAX], unsigned cap,
                         unsigned skip, bool *more)
{
    unsigned found = 0, taken = 0;
    const char *last = NULL;

    *more = false;

    /* A selection sort over the pool rather than an array to sort: n is 128
     * and the answer is usually a handful, so this walks the pool once per
     * result rather than copying and sorting the whole of it. */
    for (;;) {
        const char *best = NULL;
        unsigned i;

        for (i = 0; i < NODES; i++) {
            struct node *n = &nodes[i];

            if (!n->used || n->directory) {
                continue;
            }

            if (nwhere > 0 && !matches(n, where, nwhere)) {
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

static void reply_with(uint64_t to, const struct ram_reply *rep)
{
    struct message out;

    memset(&out, 0, sizeof(out));
    out.length = sizeof(*rep);
    memcpy(out.data, rep, sizeof(*rep));

    (void)kosmos_reply(to, &out);
}

static void fail(uint64_t to, uint32_t code)
{
    struct ram_reply rep;

    memset(&rep, 0, sizeof(rep));
    rep.error = code;
    reply_with(to, &rep);
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
    unsigned i;

    for (i = 0; i < WATCHERS; i++) {
        struct watcher *w = &watchers[i];
        struct ram_reply rep;
        bool more;

        if (!w->used) {
            continue;
        }

        memset(&rep, 0, sizeof(rep));
        rep.count = evaluate(w->where, w->nwhere, rep.u.entries,
                             RAM_ENTRIES_MAX, 0, &more);
        rep.more = more ? 1u : 0u;

        if (same_paths(rep.u.entries, rep.count, w->last, w->nlast)) {
            continue;
        }

        w->used = false;
        reply_with(w->who, &rep);
    }
}

/*------------------------------------------------------------------------
 * The operations.
 *----------------------------------------------------------------------*/

static void answer(const struct message *msg, uint64_t sender)
{
    struct ram_request req;
    struct ram_reply rep;
    char path[RAM_PATH_MAX];
    struct node *n;
    unsigned i;

    if (msg->length < sizeof(req)) {
        fail(sender, RAM_ERR_BAD_OP);
        return;
    }

    memcpy(&req, msg->data, sizeof(req));
    memset(&rep, 0, sizeof(rep));

    /* Whatever arrived, terminated. `path` is 128 bytes from another
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

            for (k = 0; k < NODES; k++) {
                const char *name;

                if (!nodes[k].used) {
                    continue;
                }

                name = child_of(path, nodes[k].path);

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

        memcpy(rep.u.data, n->value + at, take);
        rep.length = take;
        rep.packed = n->packed;
        break;
    }

    case RAM_OP_WRITE: {
        struct ram_attr size;

        if (is_root(path)) {
            fail(sender, RAM_ERR_NO_PATH);   /* the root is not a file */
            return;
        }

        if (req.length > RAM_DATA_MAX) {
            req.length = RAM_DATA_MAX;
        }

        ensure_parents(path);
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
        }

        if (req.offset + req.length > VALUE_MAX) {
            fail(sender, RAM_ERR_FULL);
            return;
        }

        memcpy(n->value + req.offset, req.u.data, req.length);

        if (req.offset + req.length > n->length) {
            n->length = req.offset + req.length;
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

        writes++;
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

        rep.count = evaluate(req.attrs, req.count, rep.u.entries,
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

        rep.count = evaluate(req.attrs, nwhere, rep.u.entries,
                             RAM_ENTRIES_MAX, 0, &more);
        rep.more = more ? 1u : 0u;

        /* Already different from what the caller has: answer now. */
        if (!same_paths(rep.u.entries, rep.count, req.u.known, known)) {
            break;
        }

        for (i = 0; i < WATCHERS; i++) {
            if (!watchers[i].used) {
                w = &watchers[i];
                break;
            }
        }

        if (w == NULL) {
            fail(sender, RAM_ERR_FULL);
            return;
        }

        memset(w, 0, sizeof(*w));
        w->used = true;
        w->who = sender;
        w->nwhere = nwhere;

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

    case RAM_OP_WATCHERS:
        for (i = 0; i < WATCHERS; i++) {
            if (watchers[i].used) {
                rep.count++;
            }
        }

        break;

    default:
        fail(sender, RAM_ERR_BAD_OP);
        return;
    }

    reply_with(sender, &rep);
}

void ramfs_server(long endpoint)
{
    memset(nodes, 0, sizeof(nodes));
    memset(watchers, 0, sizeof(watchers));

    for (;;) {
        struct message msg;
        uint64_t sender = 0;

        /* Nothing here happens on its own: a watcher is woken by a write,
         * and a write is a message. */
        if (kosmos_receive(endpoint, &msg, &sender, 0, 0) != 0) {
            return;
        }

        answer(&msg, sender);
    }
}
