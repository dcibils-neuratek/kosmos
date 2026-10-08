/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /bin: the programs carried inside the image.
 *
 * Read-only by construction - there is no filesystem behind it, only an
 * array the build put in the binary - so `binproto.h` has no `write` to
 * send it, and a program's properties cannot change while the system runs.
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
enum view { VIEW_ALL, VIEW_APPS, VIEW_PROGRAMS, VIEW_DESKBAR };

static enum view view_of(const char **name)
{
    static const struct { const char *word; enum view view; } views[] = {
        { "apps", VIEW_APPS }, { "programs", VIEW_PROGRAMS },
        { "deskbar", VIEW_DESKBAR },
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

    /* The menu's names are not the store's: `menu_path` below. */
    if (v == VIEW_DESKBAR) {
        return false;
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

/*
 * **The Deskbar's menu, as it ships** (`roadmap.md` 6zd): `/Kosmos/Deskbar`,
 * a folder a section, holding a launcher an application - made from each
 * application's header, so there is no file to keep in step with it and it
 * cannot go stale. `/Home/Deskbar` holds only what a person made, and the
 * Deskbar shows the two merged (`deskbarmenu.lua`).
 *
 * An application's place is its `section`, `applications` when it says
 * none, with the first letter made a capital - the word a person reads in
 * the menu, as the Deskbar wrote it when it copied these into every home -
 * and a group after a slash is a submenu: `demos/GLDemos` puts `glgears`
 * at `Demos/GLDemos/glgears`. Not the Deskbar itself, which is not
 * something you start, and not `section none` - a window something else
 * opens with a file in it, which has nothing to show opened from a menu.
 *
 * False when the application is not in the menu, or its place does not fit.
 */
/*
 * **A page the menu opens** (a `.page` file in `user/pages`, 3 October 2026): one in
 * the store's `pages/` that says `kosmos: page <address>` - the cheat sheet,
 * a tutorial - which is a launcher starting the browser there. Its name in
 * the menu is the file's without `pages/` and `.page`.
 */
#define PAGES_DIR  "pages/"
#define PAGE_END   ".page"

static bool is_page(const struct source_entry *e)
{
    unsigned long n = 0;
    size_t nlen = strlen(e->name);
    size_t dlen = sizeof(PAGES_DIR) - 1, elen = sizeof(PAGE_END) - 1;

    return nlen > dlen + elen && strncmp(e->name, PAGES_DIR, dlen) == 0
           && strcasecmp(e->name + nlen - elen, PAGE_END) == 0
           && strchr(e->name + dlen, '/') == NULL
           && declared(e->text, e->length, "page", &n) != NULL;
}

static bool menu_path(const struct source_entry *e, char *out, size_t cap)
{
    unsigned long n = 0;
    const char *section;
    size_t nlen = strlen(e->name);
    const char *base = e->name;
    size_t blen = nlen - 4;
    size_t at = 0;

    if (is_page(e)) {
        base = e->name + sizeof(PAGES_DIR) - 1;
        blen = nlen - (sizeof(PAGES_DIR) - 1) - (sizeof(PAGE_END) - 1);
    } else if (strchr(e->name, '/') != NULL
        || declared(e->text, e->length, "application", &n) == NULL
        || nlen < 5 || strcasecmp(e->name + nlen - 4, ".lua") != 0
        || strcasecmp(e->name, "deskbar.lua") == 0) {
        return false;
    }

    section = declared(e->text, e->length, "section", &n);

    if (section == NULL) {
        section = "applications";
        n = 12;
    }

    while (n > 0 && (section[n - 1] == ' ' || section[n - 1] == '\t'
                     || section[n - 1] == '\r')) {
        n--;
    }

    if (n == 0 || (n == 4 && strncasecmp(section, "none", 4) == 0)
        || n + 1 + blen + 1 > cap) {
        return false;
    }

    memcpy(out, section, n);
    at = n;

    if (out[0] >= 'a' && out[0] <= 'z') {
        out[0] = (char)(out[0] - 'a' + 'A');
    }

    out[at++] = '/';
    memcpy(out + at, base, blen);
    at += blen;
    out[at] = '\0';

    return true;
}

/* The application whose place in the menu is `path`, or NULL. */
static const struct source_entry *menu_find(const char *path, size_t plen)
{
    char mp[BIN_NAME_MAX];
    unsigned i;

    for (i = 0; i < store_count; i++) {
        if (menu_path(&store[i], mp, sizeof(mp)) && strlen(mp) == plen
            && strncasecmp(mp, path, plen) == 0) {
            return &store[i];
        }
    }

    return NULL;
}

/* A folder of the menu an earlier application already gave. */
static bool menu_listed_before(unsigned upto, const char *dir, size_t dlen,
                               const char *child, size_t clen)
{
    char mp[BIN_NAME_MAX];
    unsigned j;

    for (j = 0; j < upto; j++) {
        size_t l;
        bool f;
        const char *c;

        if (!menu_path(&store[j], mp, sizeof(mp))) {
            continue;
        }

        c = child_of(mp, dir, dlen, &l, &f);

        if (c != NULL && f && l == clen && strncasecmp(c, child, clen) == 0) {
            return true;
        }
    }

    return false;
}

/* Whether some application's place is below `dir`: a folder of the menu. */
static bool menu_folder(const char *dir, size_t dlen)
{
    char mp[BIN_NAME_MAX];
    unsigned i;

    for (i = 0; i < store_count; i++) {
        size_t l;
        bool f;

        if (menu_path(&store[i], mp, sizeof(mp))
            && child_of(mp, dir, dlen, &l, &f) != NULL) {
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

    /* What it opens, as the line says it; the namespace splits the words. */
    s = declared(e->text, e->length, "opens", &n);
    copy_word(rep->opens, BIN_OPENS_MAX, (s != NULL) ? s : "",
              (s != NULL) ? n : 0);

    /* Its name for a person, as the line says it. */
    s = declared(e->text, e->length, "name", &n);
    copy_word(rep->title, BIN_TITLE_MAX, (s != NULL) ? s : "",
              (s != NULL) ? n : 0);

    /*
     * `needs` is a line of words, and each becomes one entry. A program that
     * declares more than `BIN_NEEDS_MAX` gets the first few, which is the
     * one place here that quietly drops something - and the static assert
     * cannot catch it because it is the *program* that is too greedy, not
     * the protocol. Six since 24 September: the window manager declared
     * five - it holds the camera to pass it on (`usb.md` §11 8d) - and four
     * would have dropped the last without a word. Eight since 29 September,
     * when it came to seven with `midi` and `profile`, and six would have
     * dropped the right to profile the same way. Nine on 8 October with
     * `tiles`, and eight dropped the map's tiles from Maps without a word -
     * so sixteen, and `tools/check_needs.py` (in `make test`'s host suite)
     * now refuses a program that declares more than this holds, rather than
     * a fourth time being found by what stopped working.
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

/*
 * **A launcher in the shipped menu**: `kind` launcher, the application's
 * picture, and the application it starts - its name in the store, in
 * `data`, which the namespace turns into the path it has there
 * (`/Kosmos/Apps/clock.lua`). What a launcher in `/Home/Deskbar` says in
 * its attributes, said here from the header.
 */
static void fill_launcher(const struct source_entry *e, struct bin_reply *rep)
{
    unsigned long n = 0;
    const char *s = declared(e->text, e->length, "icon", &n);
    size_t len = strlen(e->name);

    copy_word(rep->kind, BIN_WORD_MAX, "launcher", 8);
    copy_word(rep->icon, BIN_ICON_MAX, (s != NULL) ? s : "",
              (s != NULL) ? n : 0);

    /* The application's name for a person, which the menu shows; the
     * launcher's own name in the store stays the file's. */
    s = declared(e->text, e->length, "name", &n);
    copy_word(rep->title, BIN_TITLE_MAX, (s != NULL) ? s : "",
              (s != NULL) ? n : 0);

    /*
     * A page starts the browser, and its address follows the program's
     * name after a NUL - in `data`, which has the room, rather than a field
     * every reply would carry for two launchers. The namespace splits them.
     */
    if (is_page(e)) {
        static const char browser[] = "browser.lua";
        const char *page = declared(e->text, e->length, "page", &n);
        size_t plen = (page != NULL) ? n : 0;

        while (plen > 0 && (page[plen - 1] == ' ' || page[plen - 1] == '\r'
                            || page[plen - 1] == '\t')) {
            plen--;
        }

        if (sizeof(browser) + plen > BIN_CHUNK) {
            plen = BIN_CHUNK - sizeof(browser);
        }

        memcpy(rep->data, browser, sizeof(browser));      /* with its NUL */
        memcpy(rep->data + sizeof(browser), page, plen);
        rep->length = (uint32_t)(sizeof(browser) + plen);
        return;
    }

    if (len > BIN_CHUNK) {
        len = BIN_CHUNK;
    }

    memcpy(rep->data, e->name, len);
    rep->length = (uint32_t)len;
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
            char mp[BIN_NAME_MAX];

            if (view == VIEW_DESKBAR) {
                /* The menu's folders and launchers, by each application's
                 * place in it rather than by its name in the store. */
                bool folder;

                if (!menu_path(&store[i], mp, sizeof(mp))) {
                    continue;
                }

                shown = child_of(mp, dir, dlen, &slen, &folder);

                if (shown == NULL
                    || (folder
                        && menu_listed_before(i, dir, dlen, shown, slen))) {
                    continue;
                }
            } else if (view != VIEW_ALL) {
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

        /* A launcher is an empty file: what it starts is its attributes. */
        if (view == VIEW_DESKBAR) {
            const char *p = (at[0] == '/') ? at + 1 : at;

            if (menu_find(p, strlen(p)) == NULL) {
                rep->error = BIN_ERR_NO_PROGRAM;
            }

            break;
        }

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
        if (view == VIEW_DESKBAR) {
            const char *p = (at[0] == '/') ? at + 1 : at;
            size_t plen = strlen(p);

            while (plen > 0 && p[plen - 1] == '/') {
                plen--;
            }

            e = menu_find(p, plen);

            if (e != NULL) {
                fill_launcher(e, rep);
            } else if (plen == 0 || menu_folder(p, plen)) {
                copy_word(rep->kind, BIN_WORD_MAX, "directory", 9);
            } else {
                rep->error = BIN_ERR_NO_PROGRAM;
            }

            break;
        }

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
