/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /bin: the programs carried inside the image.
 *
 * Read-only by construction - there is no filesystem behind it, only an
 * array the build put in the binary - so `write` is refused rather than
 * unimplemented, and a program's properties cannot change while the system
 * runs.
 *
 * **Its one interesting job is deciding what a program is.** A file says so
 * itself, in its opening comment block: `kosmos: application` means it draws
 * a window, `kosmos: section <name>` says where in the Deskbar's menu it
 * belongs, and `kosmos: needs <words>` declares the authorities a launcher
 * should grant without having to read the source. Guessing instead - looking
 * for `ui.window`, say - would be a store deciding what a program is by
 * reading it, and would be wrong the first time somebody wrote the name in a
 * comment.
 *
 * **The header, not the file.** The block ends at the first line that is not
 * a comment, which matters: this once scanned the whole source, so a program
 * with those words in a string declared an authority by accident.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "kosmos.h"
#include "source.h"
#include "binproto.h"

/*
 * Two stores, one server.
 *
 * `/bin` and `/Kosmos/Libraries` differ only in which array they serve - the Lua original
 * had the same property and used one function for both roles, which is worth
 * keeping rather than discovering again. The store is set once at entry and
 * never changes.
 */
extern const struct source_entry programs_lua_table[];
extern const unsigned            programs_lua_count;
extern const struct source_entry libraries_lua_table[];
extern const unsigned            libraries_lua_count;

static const struct source_entry *store;
static unsigned                   store_count;
static bool                       libraries;     /* which store this serves */

/* Whatever its case (`roadmap.md` 6s): `/Kosmos/Apps/Clock.lua` is `clock.lua`. */
static const struct source_entry *find(const char *name)
{
    unsigned i;

    for (i = 0; i < store_count; i++) {
        if (strcasecmp(store[i].name, name) == 0) {
            return &store[i];
        }
    }

    return NULL;
}

/* Where the opening comment block ends: the first line that does not begin
 * with `--`, blank lines included as part of it. */
static unsigned long header_end(const char *src, unsigned long len)
{
    unsigned long at = 0;

    while (at < len) {
        unsigned long eol = at;

        while (eol < len && src[eol] != '\n') {
            eol++;
        }

        if (!(eol - at == 0
              || (eol - at >= 2 && src[at] == '-' && src[at + 1] == '-'))) {
            return at;
        }

        at = eol + 1;
    }

    return len;
}

/*
 * `kosmos: <what> <rest of the line>`, inside the header only.
 *
 * Returns the bytes after the keyword, or NULL. No allocation and no copy -
 * the caller takes what it needs out of the source where it lies.
 */
static const char *declared(const char *src, unsigned long len,
                            const char *what, unsigned long *out_len)
{
    unsigned long stop = header_end(src, len);
    unsigned long at = 0;
    unsigned long wlen = strlen(what);

    while (at + 8 < stop) {
        if (memcmp(src + at, "kosmos:", 7) == 0) {
            unsigned long p = at + 7;

            while (p < stop && (src[p] == ' ' || src[p] == '\t')) {
                p++;
            }

            if (p + wlen <= stop && memcmp(src + p, what, wlen) == 0) {
                unsigned long e;

                p += wlen;

                while (p < stop && (src[p] == ' ' || src[p] == '\t')) {
                    p++;
                }

                e = p;

                while (e < stop && src[e] != '\n') {
                    e++;
                }

                *out_len = e - p;
                return src + p;
            }
        }

        at++;
    }

    return NULL;
}

/*
 * **Which of the image's files a mount shows** (`roadmap.md` 6s c2). One
 * store holds the applications and the programs, as `user/bin/` does, and
 * the layout shows them as two folders: `/Kosmos/Apps`, what opens a
 * window, and `/Kosmos/Programs`, the rest. The namespace puts a mount's
 * root in front of every path it sends, so the view is the name's first
 * part - `/apps/clock.lua` - and a name with neither is the whole store,
 * as the libraries are served.
 */
enum view { VIEW_ALL, VIEW_APPS, VIEW_PROGRAMS };

static enum view view_of(const char **name)
{
    static const struct { const char *word; enum view view; } views[] = {
        { "apps", VIEW_APPS }, { "programs", VIEW_PROGRAMS },
    };
    const char *n = *name;
    unsigned i;

    if (*n == '/') {
        n++;
    }

    for (i = 0; i < sizeof(views) / sizeof(views[0]); i++) {
        size_t len = strlen(views[i].word);

        if (strncasecmp(n, views[i].word, len) == 0
            && (n[len] == '\0' || n[len] == '/')) {
            *name = n + len;
            return views[i].view;
        }
    }

    return VIEW_ALL;
}

static bool in_view(const struct source_entry *e, enum view v)
{
    unsigned long n = 0;
    bool app;

    if (v == VIEW_ALL) {
        return true;
    }

    /* The applications and the programs are the store's top level: what
     * is in a folder of it - the looks, in `themes/` - is neither. */
    if (strchr(e->name, '/') != NULL) {
        return false;
    }

    app = declared(e->text, e->length, "application", &n) != NULL;

    return (v == VIEW_APPS) ? app : !app;
}

/*
 * **Folders** (`roadmap.md` 6s c3b). A store is flat - each entry one name,
 * `ui.lua` or `luacheck/check.lua` or `themes/Plex.theme` - and it used to
 * be listed flat, so `ls /Kosmos/Libraries` said `luacheck/check.lua` as
 * though that were one name. A folder is now the first part of the names
 * below it, listed once, and a path naming one answers as a directory.
 *
 * `name` is inside `dir` ("" the top) when it is `dir/...`; what it is
 * called there is its next part, `*folder` when more follows. NULL when it
 * is not inside.
 */
static const char *child_of(const char *name, const char *dir, size_t dlen,
                            size_t *clen, bool *folder)
{
    const char *rest = name;
    const char *slash;

    if (dlen > 0) {
        if (strncasecmp(name, dir, dlen) != 0 || name[dlen] != '/') {
            return NULL;
        }

        rest = name + dlen + 1;
    }

    slash = strchr(rest, '/');
    *clen = (slash != NULL) ? (size_t)(slash - rest) : strlen(rest);
    *folder = (slash != NULL);

    return (*clen > 0) ? rest : NULL;
}

/* A folder an earlier entry already gave: listed once, not once a file. */
static bool listed_before(unsigned upto, const char *dir, size_t dlen,
                          const char *child, size_t clen)
{
    unsigned j;

    for (j = 0; j < upto; j++) {
        size_t l;
        bool f;
        const char *c = child_of(store[j].name, dir, dlen, &l, &f);

        if (c != NULL && f && l == clen && strncasecmp(c, child, clen) == 0) {
            return true;
        }
    }

    return false;
}

/* Whether some entry is below `dir`: a folder, though no entry is called it. */
static bool is_folder(const char *dir, size_t dlen)
{
    unsigned i;

    for (i = 0; i < store_count; i++) {
        size_t l;
        bool f;

        if (child_of(store[i].name, dir, dlen, &l, &f) != NULL) {
            return true;
        }
    }

    return false;
}

static void copy_word(char *dst, unsigned cap, const char *src,
                      unsigned long len)
{
    if (len >= cap) {
        len = cap - 1;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void fill_attrs(const struct source_entry *e, struct bin_reply *rep)
{
    unsigned long n = 0;
    const char *s;

    rep->size = (uint32_t)e->length;

    /* What it is, said as it is (`roadmap.md` 6s c3b): a file that is not
     * Lua - a look, `Plex.theme` - is a file, and a library is a library.
     * Both used to report `program`, which is what the rest of this reads
     * a header for. */
    {
        size_t len = strlen(e->name);

        if (len < 4 || strcasecmp(e->name + len - 4, ".lua") != 0) {
            copy_word(rep->kind, BIN_WORD_MAX, "file", 4);
            return;
        }
    }

    if (libraries) {
        copy_word(rep->kind, BIN_WORD_MAX, "library", 7);
        return;
    }

    s = declared(e->text, e->length, "application", &n);
    rep->windowed = (s != NULL) ? 1u : 0u;

    if (rep->windowed) {
        copy_word(rep->kind, BIN_WORD_MAX, "application", 11);

        s = declared(e->text, e->length, "section", &n);
        copy_word(rep->section, BIN_WORD_MAX,
                  (s != NULL) ? s : "applications",
                  (s != NULL) ? n : 12);
    } else {
        copy_word(rep->kind, BIN_WORD_MAX, "program", 7);
    }

    /*
     * The icon, for applications and programs alike: a program has no menu
     * row today, and declaring a face costs it nothing if one arrives. Blank
     * when none is declared, and whoever draws it picks a generic one.
     */
    s = declared(e->text, e->length, "icon", &n);
    copy_word(rep->icon, BIN_ICON_MAX, (s != NULL) ? s : "",
              (s != NULL) ? n : 0);

    /*
     * `needs` is a line of words, and each becomes one entry. A program that
     * declares more than `BIN_NEEDS_MAX` gets the first few, which is the
     * one place here that quietly drops something - and the static assert
     * cannot catch it because it is the *program* that is too greedy, not
     * the protocol. Six since 24 September: the window manager declares
     * five - it holds the camera to pass it on (`usb.md` §11 8d) - and four
     * would have dropped the last without a word.
     */
    s = declared(e->text, e->length, "needs", &n);

    if (s != NULL) {
        unsigned long at = 0;
        unsigned slot = 0;

        while (at < n && slot < BIN_NEEDS_MAX) {
            unsigned long start;

            while (at < n && (s[at] == ' ' || s[at] == '\t')) {
                at++;
            }

            start = at;

            while (at < n && s[at] != ' ' && s[at] != '\t') {
                at++;
            }

            if (at > start) {
                copy_word(rep->needs[slot++], BIN_WORD_MAX, s + start,
                          at - start);
            }
        }
    }
}

static void answer(const struct message *in, uint64_t sender)
{
    struct message out;
    struct bin_reply *rep = (struct bin_reply *)(void *)out.data;
    const struct bin_request *req =
        (const struct bin_request *)(const void *)in->data;
    const struct source_entry *e;
    char name[BIN_NAME_MAX];
    const char *at;
    enum view view;

    memset(&out, 0, sizeof(out));
    out.tag = in->tag;
    out.length = (uint32_t)sizeof(*rep);

    if (in->length < sizeof(*req)) {
        rep->error = BIN_ERR_BAD_OP;
        (void)kosmos_reply(sender, &out);
        return;
    }

    /* The mount prefix is stripped before this arrives; a leading slash is
     * not, so "/ls.lua" and "ls.lua" both name the same program. A view,
     * when the mount has one, is the first part of what is left. */
    memcpy(name, req->name, BIN_NAME_MAX);
    name[BIN_NAME_MAX - 1] = '\0';
    at = name;
    view = view_of(&at);

    switch (req->op) {
    case BIN_OP_LIST: {
        /*
         * From wherever the caller left off, and saying whether there is
         * more - the same shape `read` below has always had.
         *
         * It used to start at nought every time and stop when the reply was
         * full, which silently answered with the first 74 names of 82. The
         * eight it dropped were the last alphabetically, and four of them
         * were applications: the Deskbar listed what it was told and could
         * not offer Tracker, the Terminal, the top bar or the web server.
         * Nothing failed. The menu was simply short, and had been since the
         * seventy-fifth program was added.
         *
         * The guard underneath this was `BIN_CHUNK / BIN_NAME_MAX >= 64`,
         * asserting "a list must hold every program in the image" - which a
         * static assert cannot know, because the number of programs is not
         * a number it can see. It was checking the wrong side of the
         * inequality: the chunk shrinking, rather than the image growing.
         */
        /* The offset is how many of *this view's* names the caller already
         * has - it counts what it was sent - so the ones the view leaves
         * out are skipped without being counted. With no view, the names
         * are the children of the folder asked for, a folder once. */
        unsigned i, seen = 0;
        const char *dir = (at[0] == '/') ? at + 1 : at;
        size_t dlen = strlen(dir);

        while (dlen > 0 && dir[dlen - 1] == '/') {
            dlen--;
        }

        for (i = 0; i < store_count; i++) {
            const char *shown = store[i].name;
            size_t slen = strlen(shown);

            if (view != VIEW_ALL) {
                if (!in_view(&store[i], view)) {
                    continue;
                }
            } else {
                bool folder;

                shown = child_of(store[i].name, dir, dlen, &slen, &folder);

                if (shown == NULL
                    || (folder && listed_before(i, dir, dlen, shown, slen))) {
                    continue;
                }
            }

            if (seen++ < req->offset) {
                continue;
            }

            if (rep->count >= BIN_CHUNK / BIN_NAME_MAX) {
                rep->more = 1u;
                break;
            }

            copy_word((char *)rep->data + rep->count * BIN_NAME_MAX,
                      BIN_NAME_MAX, shown, slen);
            rep->count++;
        }

        break;
    }

    case BIN_OP_READ: {
        unsigned long left;

        e = find((at[0] == '/') ? at + 1 : at);

        /* A program asked for under `/Kosmos/Apps` is not there. */
        if (e == NULL || !in_view(e, view)) {
            rep->error = BIN_ERR_NO_PROGRAM;
            break;
        }

        rep->size = (uint32_t)e->length;

        if (req->offset >= e->length) {
            rep->length = 0;
            break;
        }

        left = e->length - req->offset;
        rep->length = (uint32_t)((left > BIN_CHUNK) ? BIN_CHUNK : left);
        memcpy(rep->data, e->text + req->offset, rep->length);
        rep->more = (req->offset + rep->length < e->length) ? 1u : 0u;
        break;
    }

    case BIN_OP_GETATTR:
        e = find((at[0] == '/') ? at + 1 : at);

        /* A folder of the store: `luacheck`, `themes`. */
        if (e == NULL && view == VIEW_ALL) {
            const char *dir = (at[0] == '/') ? at + 1 : at;

            if (dir[0] != '\0' && is_folder(dir, strlen(dir))) {
                copy_word(rep->kind, BIN_WORD_MAX, "directory", 9);
                break;
            }
        }

        if (e == NULL || !in_view(e, view)) {
            rep->error = BIN_ERR_NO_PROGRAM;
            break;
        }

        fill_attrs(e, rep);
        break;

    default:
        rep->error = BIN_ERR_BAD_OP;
        break;
    }

    (void)kosmos_reply(sender, &out);
}

void binfs_server(long endpoint, int serving_libraries)
{
    libraries = (serving_libraries != 0);

    if (libraries) {
        store = libraries_lua_table;
        store_count = libraries_lua_count;
    } else {
        store = programs_lua_table;
        store_count = programs_lua_count;
    }

    for (;;) {
        struct message msg;
        uint64_t sender = 0;

        /* Blocking with no deadline: /bin is in the image and nothing here
         * happens on its own, so there is nothing to wake up for. */
        if (kosmos_receive(endpoint, &msg, &sender, 0, 0) != 0) {
            return;
        }

        answer(&msg, sender);
    }
}
