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

#include "kosmos.h"
#include "source.h"
#include "binproto.h"

/*
 * Two stores, one server.
 *
 * `/bin` and `/lib` differ only in which array they serve - the Lua original
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

static const struct source_entry *find(const char *name)
{
    unsigned i;

    for (i = 0; i < store_count; i++) {
        if (strcmp(store[i].name, name) == 0) {
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
     * `needs` is a line of words, and each becomes one entry. A program that
     * declares more than `BIN_NEEDS_MAX` gets the first few, which is the
     * one place here that quietly drops something - and the static assert
     * cannot catch it because it is the *program* that is too greedy, not
     * the protocol. Four is more than any program has ever asked for.
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

    memset(&out, 0, sizeof(out));
    out.tag = in->tag;
    out.length = (uint32_t)sizeof(*rep);

    if (in->length < sizeof(*req)) {
        rep->error = BIN_ERR_BAD_OP;
        (void)kosmos_reply(sender, &out);
        return;
    }

    /* The mount prefix is stripped before this arrives; a leading slash is
     * not, so "/ls.lua" and "ls.lua" both name the same program. */
    memcpy(name, req->name, BIN_NAME_MAX);
    name[BIN_NAME_MAX - 1] = '\0';

    switch (req->op) {
    case BIN_OP_LIST: {
        unsigned i;

        for (i = 0; i < store_count && rep->count < BIN_CHUNK / BIN_NAME_MAX;
             i++) {
            copy_word((char *)rep->data + rep->count * BIN_NAME_MAX,
                      BIN_NAME_MAX, store[i].name, strlen(store[i].name));
            rep->count++;
        }

        break;
    }

    case BIN_OP_READ: {
        unsigned long left;

        e = find((name[0] == '/') ? name + 1 : name);

        if (e == NULL) {
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
        e = find((name[0] == '/') ? name + 1 : name);

        if (e == NULL) {
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

void binfs_server(long endpoint, int libraries)
{
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
