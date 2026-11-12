/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **`glob` and `fnmatch`, for ported code** (`docs/w3.md`): the names a
 * pattern matches, read folder by folder through `opendir` and `readdir`,
 * so they resolve against the process's own namespace like every path the
 * POSIX layer takes. libxml2's `runtest` finds its cases this way, and
 * without it its lists were empty and it passed with nothing tested.
 *
 * A pattern is taken a part at a time. A part with no wildcard is joined
 * on as it is written; a part with one is held against every name in the
 * folder so far. A name is returned spelled as the pattern spelled its
 * path - `./test/a.xml` for `./test/` and a star - since a caller builds other
 * names from it.
 */

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 *--------------------------------------------------------------------------
 * fnmatch.
 */

/* One bracket expression at `p`, the `[` already passed, against `c`.
 * Returns where the expression ends, or NULL when it is not closed and the
 * `[` is therefore an ordinary character. */
static const char *bracket(const char *p, char c, int flags, int *matched)
{
    int negate = 0;
    int first = 1;

    *matched = 0;

    if (*p == '!' || *p == '^') {
        negate = 1;
        p++;
    }

    while (*p != '\0' && (*p != ']' || first)) {
        char lo = *p++;
        char hi;

        first = 0;

        if (lo == '\\' && !(flags & FNM_NOESCAPE) && *p != '\0') {
            lo = *p++;
        }

        if ((flags & FNM_PATHNAME) && lo == '/') {
            return NULL;                /* a slash is never in a bracket */
        }

        hi = lo;

        if (*p == '-' && p[1] != ']' && p[1] != '\0') {
            p++;
            hi = *p++;

            if (hi == '\\' && !(flags & FNM_NOESCAPE) && *p != '\0') {
                hi = *p++;
            }
        }

        if ((unsigned char)c >= (unsigned char)lo && (unsigned char)c <= (unsigned char)hi) {
            *matched = 1;
        }
    }

    if (*p != ']') {
        return NULL;
    }

    if (negate) {
        *matched = !*matched;
    }

    return p + 1;
}

static int match(const char *p, const char *s, const char *start, int flags)
{
    for (;;) {
        char c = *p++;

        switch (c) {
        case '\0':
            return *s == '\0' ? 0 : FNM_NOMATCH;

        case '?':
            if (*s == '\0' || ((flags & FNM_PATHNAME) && *s == '/')
                || ((flags & FNM_PERIOD) && *s == '.'
                    && (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/')))) {
                return FNM_NOMATCH;
            }

            s++;
            break;

        case '*':
            if ((flags & FNM_PERIOD) && *s == '.'
                && (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/'))) {
                return FNM_NOMATCH;
            }

            while (*p == '*') {
                p++;
            }

            if (*p == '\0') {
                return (flags & FNM_PATHNAME) && strchr(s, '/') != NULL ? FNM_NOMATCH : 0;
            }

            /* Every place the rest could begin, shortest first. The
             * period rule goes on with it: past a slash, a name begins
             * again, and `*' + '/' + '*` does not match `a/.b`. */
            for (; *s != '\0'; s++) {
                if (match(p, s, start, flags) == 0) {
                    return 0;
                }

                if ((flags & FNM_PATHNAME) && *s == '/') {
                    return FNM_NOMATCH;
                }
            }

            return match(p, s, start, flags);

        case '[': {
            int matched;
            const char *after;

            if (*s == '\0') {
                return FNM_NOMATCH;
            }

            if ((flags & FNM_PERIOD) && *s == '.'
                && (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/'))) {
                return FNM_NOMATCH;
            }

            after = bracket(p, *s, flags, &matched);

            /* Not closed: POSIX makes the `[` an ordinary character, as
             * glibc does. BSD's, this Mac's, refuses the pattern instead
             * (`tools/test_glob.c` holds the difference to these cases). */
            if (after == NULL) {
                if (*s != '[') {
                    return FNM_NOMATCH;
                }
            } else if (!matched) {
                return FNM_NOMATCH;
            } else {
                p = after;
            }

            s++;
            break;
        }

        case '\\':
            /* A backslash quoting nothing: POSIX leaves it open, and this
             * says no match, as BSD's and glibc's do. */
            if (!(flags & FNM_NOESCAPE)) {
                if (*p == '\0') {
                    return FNM_NOMATCH;
                }

                c = *p++;
            }
            /* fall through */

        default:
            if (c != *s) {
                return FNM_NOMATCH;
            }

            s++;
            break;
        }
    }
}

int fnmatch(const char *pattern, const char *string, int flags)
{
    if (pattern == NULL || string == NULL) {
        return FNM_NOMATCH;
    }

    return match(pattern, string, string, flags);
}

/*
 *--------------------------------------------------------------------------
 * glob.
 */

struct search {
    int    flags;
    int  (*errfunc)(const char *, int);
    glob_t *found;
    size_t room;
    int    failed;                      /* 0, GLOB_NOSPACE or GLOB_ABORTED */
};

static int magic(const char *part, size_t n, int flags)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (part[i] == '*' || part[i] == '?' || part[i] == '[') {
            return 1;
        }

        if (part[i] == '\\' && !(flags & GLOB_NOESCAPE) && i + 1 < n) {
            i++;
        }
    }

    return 0;
}

static int add(struct search *s, const char *path)
{
    glob_t *g = s->found;
    size_t at = g->gl_offs + g->gl_pathc;
    char *copy;

    if (at + 2 > s->room) {
        size_t room = s->room < 16 ? 16 : s->room * 2;
        char **v = realloc(g->gl_pathv, room * sizeof(*v));

        if (v == NULL) {
            return s->failed = GLOB_NOSPACE;
        }

        g->gl_pathv = v;
        s->room = room;
    }

    if ((copy = strdup(path)) == NULL) {
        return s->failed = GLOB_NOSPACE;
    }

    g->gl_pathv[at] = copy;
    g->gl_pathv[at + 1] = NULL;
    g->gl_pathc++;
    return 0;
}

/* A part with its quoting taken out, for a part with no wildcard. */
static void unquote(char *to, const char *part, size_t n, int flags)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (part[i] == '\\' && !(flags & GLOB_NOESCAPE) && i + 1 < n) {
            i++;
        }

        *to++ = part[i];
    }

    *to = '\0';
}

static int is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* `path` holds what matched so far, `len` bytes of it; `rest` is the
 * pattern still to match, starting at a part. */
static void walk(struct search *s, char *path, size_t len, size_t size, const char *rest)
{
    const char *end;
    size_t n;

    while (*rest == '/') {              /* the slashes the pattern wrote */
        if (len + 1 >= size) {
            return;
        }

        path[len++] = *rest++;
        path[len] = '\0';
    }

    if (*rest == '\0') {                /* every part matched */
        struct stat st;

        if (len == 0 || stat(path, &st) != 0) {
            return;
        }

        if ((s->flags & GLOB_MARK) && S_ISDIR(st.st_mode) && path[len - 1] != '/'
            && len + 1 < size) {
            path[len] = '/';
            path[len + 1] = '\0';
            add(s, path);
            path[len] = '\0';
            return;
        }

        add(s, path);
        return;
    }

    end = strchr(rest, '/');
    n = end != NULL ? (size_t)(end - rest) : strlen(rest);

    if (!magic(rest, n, s->flags)) {
        if (len + n >= size) {
            return;
        }

        unquote(path + len, rest, n, s->flags);
        walk(s, path, strlen(path), size, rest + n);
        path[len] = '\0';
        return;
    }

    {
        char part[256];
        DIR *d;
        struct dirent *e;
        int fnflags = FNM_PERIOD | ((s->flags & GLOB_NOESCAPE) ? FNM_NOESCAPE : 0);

        if (n >= sizeof(part)) {
            return;
        }

        memcpy(part, rest, n);
        part[n] = '\0';

        if ((d = opendir(len == 0 ? "." : path)) == NULL) {
            if (errno != ENOENT && errno != ENOTDIR
                && ((s->errfunc != NULL && s->errfunc(len == 0 ? "." : path, errno) != 0)
                    || (s->flags & GLOB_ERR))) {
                s->failed = GLOB_ABORTED;
            }

            return;
        }

        while (s->failed == 0 && (e = readdir(d)) != NULL) {
            size_t m = strlen(e->d_name);

            if (fnmatch(part, e->d_name, fnflags) != 0 || len + m >= size) {
                continue;
            }

            memcpy(path + len, e->d_name, m + 1);

            /* A part with more after it has to be a folder. */
            if (end == NULL || is_dir(path)) {
                walk(s, path, len + m, size, rest + n);
            }

            path[len] = '\0';
        }

        closedir(d);
    }
}

static int ascending(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int glob(const char *pattern, int flags,
         int (*errfunc)(const char *path, int error), glob_t *found)
{
    struct search s;
    char path[1024];
    size_t before;

    if (pattern == NULL || found == NULL) {
        errno = EINVAL;
        return GLOB_ABORTED;
    }

    if (!(flags & GLOB_APPEND)) {
        found->gl_pathc = 0;
        found->gl_pathv = NULL;

        if (!(flags & GLOB_DOOFFS)) {
            found->gl_offs = 0;
        }
    }

    s.flags = flags;
    s.errfunc = errfunc;
    s.found = found;
    s.failed = 0;
    s.room = found->gl_pathv == NULL ? 0 : found->gl_offs + found->gl_pathc + 1;

    if (found->gl_pathv == NULL && found->gl_offs > 0) {
        size_t i;

        s.room = found->gl_offs + 1;

        if ((found->gl_pathv = calloc(s.room, sizeof(char *))) == NULL) {
            return GLOB_NOSPACE;
        }

        for (i = 0; i < s.room; i++) {
            found->gl_pathv[i] = NULL;
        }
    }

    before = found->gl_pathc;
    path[0] = '\0';
    walk(&s, path, 0, sizeof(path), pattern);

    if (s.failed != 0) {
        return s.failed;
    }

    if (found->gl_pathc == before) {
        if (!(flags & GLOB_NOCHECK)) {
            return GLOB_NOMATCH;
        }

        if (add(&s, pattern) != 0) {
            return GLOB_NOSPACE;
        }
    }

    if (!(flags & GLOB_NOSORT)) {
        qsort(found->gl_pathv + found->gl_offs + before, found->gl_pathc - before,
              sizeof(char *), ascending);
    }

    return 0;
}

void globfree(glob_t *found)
{
    size_t i;

    if (found == NULL || found->gl_pathv == NULL) {
        return;
    }

    for (i = 0; i < found->gl_pathc; i++) {
        free(found->gl_pathv[found->gl_offs + i]);
    }

    free(found->gl_pathv);
    found->gl_pathv = NULL;
    found->gl_pathc = 0;
}
