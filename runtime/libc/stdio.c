/*
 * The stream functions, all of which fail.
 *
 * See the comment in <stdio.h> for why this is the answer rather than a
 * placeholder: there is no global tree, so there is no path to open. The
 * three stream pointers are NULL and never dereferenced, because nothing
 * gets far enough to dereference them.
 */

#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

FILE *stdin  = NULL;
FILE *stdout = NULL;
FILE *stderr = NULL;

#define ENOENT  2

/*
 *--------------------------------------------------------------------------
 * Files a process already has.
 *
 * `fopen` used to fail for everything, on the reasoning in <stdio.h>: there
 * is no global tree, so there is no path to open. That reasoning is right
 * and it is not the whole of it.
 *
 * `CLAUDE.md` says a libc that lives in the app's address space and whose
 * I/O resolves against *that process's* namespace is fine and necessary.
 * The namespace is reachable from Lua and not from here - but a process can
 * read a file through it and then hand the bytes to its own libc, and from
 * then on `fopen` has something to open. That is not a global tree; it is
 * this process saying "this is what that name means to me", which is the
 * whole idea of a namespace expressed one file at a time.
 *
 * Read-only, in memory, and a handful of them. Writing is a different
 * question with a different answer - it has to go back out through the
 * namespace - and is still refused.
 *
 * The caller keeps the bytes alive. They are usually a region, because the
 * things worth doing this for are the things too big for the Lua heap.
 *--------------------------------------------------------------------------
 */

#define OPEN_MAX     4
#define PROVIDED_MAX 4

struct provided {
    const char *name;
    const unsigned char *bytes;
    size_t len;
};

static struct provided provided[PROVIDED_MAX];

struct _kosmos_file {
    const unsigned char *bytes;
    size_t len;
    size_t at;
    int    used;
};

static struct _kosmos_file open_files[OPEN_MAX];

/* The part after the last slash, because a process names a file and a
 * caller may hand back a path it built around that name. */
static const char *basename_of(const char *path)
{
    const char *last = path;

    for (; *path != '\0'; path++) {
        if (*path == '/') {
            last = path + 1;
        }
    }

    return last;
}

/*
 * `kosmos_provide(name, bytes, len)` - this name, these bytes.
 *
 * Replacing an entry with the same name rather than adding a second, so
 * that providing twice is providing, not leaking a slot.
 */
int kosmos_provide(const char *name, const void *bytes, size_t len)
{
    int i;
    int free_slot = -1;

    for (i = 0; i < PROVIDED_MAX; i++) {
        if (provided[i].name != NULL
            && strcmp(provided[i].name, name) == 0) {
            provided[i].bytes = bytes;
            provided[i].len = len;

            return 0;
        }

        if (provided[i].name == NULL && free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot < 0) {
        return -1;
    }

    provided[free_slot].name = name;
    provided[free_slot].bytes = bytes;
    provided[free_slot].len = len;

    return 0;
}

FILE *fopen(const char *path, const char *mode)
{
    const char *want = basename_of(path);
    int i, j;

    /* Reading only. A write has to go back out through the namespace, and
     * this cannot reach it. */
    if (mode != NULL && (mode[0] == 'w' || mode[0] == 'a')) {
        errno = ENOENT;
        return NULL;
    }

    for (i = 0; i < PROVIDED_MAX; i++) {
        if (provided[i].name == NULL
            || strcmp(provided[i].name, want) != 0) {
            continue;
        }

        for (j = 0; j < OPEN_MAX; j++) {
            if (open_files[j].used) {
                continue;
            }

            open_files[j].bytes = provided[i].bytes;
            open_files[j].len = provided[i].len;
            open_files[j].at = 0;
            open_files[j].used = 1;

            return &open_files[j];
        }

        break;                          /* no handle free */
    }

    errno = ENOENT;
    return NULL;
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    (void)path;
    (void)mode;
    (void)f;
    errno = ENOENT;
    return NULL;
}

int fclose(FILE *f)
{
    if (f == NULL) {
        return EOF;
    }

    f->used = 0;

    return 0;
}

size_t fread(void *b, size_t size, size_t count, FILE *f)
{
    size_t want, left;

    if (f == NULL || f->bytes == NULL || size == 0) {
        return 0;
    }

    want = size * count;
    left = f->len - f->at;

    if (want > left) {
        want = left;
    }

    memcpy(b, f->bytes + f->at, want);
    f->at += want;

    /* Elements, not bytes: a short read of half an element reports the
     * whole elements that fitted, which is what the standard says and what
     * every caller assumes. */
    return want / size;
}
size_t fwrite(const void *b, size_t s, size_t c, FILE *f)
{
    (void)b; (void)s; (void)c; (void)f;
    return 0;
}
int  feof(FILE *f)         { return f == NULL || f->at >= f->len; }
int  ferror(FILE *f)       { (void)f; return 0; }
void clearerr(FILE *f)     { (void)f; }
int  fflush(FILE *f)       { (void)f; return 0; }
int  getc(FILE *f)
{
    if (f == NULL || f->bytes == NULL || f->at >= f->len) {
        return EOF;
    }

    return f->bytes[f->at++];
}
int  ungetc(int c, FILE *f) { (void)c; (void)f; return EOF; }

char *strerror(int errnum)
{
    switch (errnum) {
    case 0:      return "no error";
    case ENOENT: return "no such path in this namespace";
    case EDOM:   return "argument outside the domain";
    case ERANGE: return "result outside the representable range";
    default:     return "unknown error";
    }
}

/*
 *--------------------------------------------------------------------------
 * The functions that print, added because a link error asked.
 *
 * The asker was Doom, which uses `fprintf(stderr, ...)` ninety-two times to
 * say what it is doing - which IWAD it found, how many lumps, what it could
 * not load. Silencing those with a stub would be throwing away the only
 * account of a failure a port like this is going to get, so they work.
 *
 * Everything goes to `SYS_WRITE`, which is the same door every other print
 * in this system goes through and lands wherever this process's console is.
 * The stream argument is *ignored*: `stdout` and `stderr` are both NULL
 * here because there is no global tree to open one from, and a process has
 * one console rather than two. Pretending otherwise would mean inventing a
 * distinction this system does not have.
 *
 * Formatting is `vsnprintf` into a fixed buffer, which is the honest limit:
 * a line longer than this is truncated rather than heap-allocated. Doom's
 * longest is a path.
 *--------------------------------------------------------------------------
 */

#include <stdarg.h>
#include <kosmos.h>

#define PRINT_MAX 512

/*
 * Where the output goes when the console will not take it.
 *
 * `SYS_WRITE` is refused unless the process owns the console, and that is
 * deliberate - it is what stops the console server being ceremony, because
 * if any process could print then nothing would depend on going through it.
 * The consequence is that C output from a *windowed* application vanishes
 * without a trace, which is how forty thousand lines of Doom managed to
 * start up, fail, and say nothing at all.
 *
 * So a refused write goes into a ring instead, and something that can reach
 * a console - the Lua side, which has the namespace - drains it and prints
 * it. Not a log file and not a buffer that grows: a fixed ring that drops
 * the oldest, because a program that prints faster than anyone reads must
 * not be able to exhaust a heap by talking.
 *
 * A ring rather than "keep the newest N bytes" so that a *drain* is cheap
 * and ordered. Losing the oldest is the right end to lose: the last thing
 * printed before a failure is the one that says what happened.
 */
#define SPILL_MAX 4096

static char   spill[SPILL_MAX];
static size_t spill_head, spill_tail;

static void spill_put(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        size_t next = (spill_head + 1) % SPILL_MAX;

        spill[spill_head] = s[i];
        spill_head = next;

        if (spill_head == spill_tail) {
            spill_tail = (spill_tail + 1) % SPILL_MAX;
        }
    }
}

/*
 * Up to `max` bytes of it, oldest first. Returns how many.
 *
 * Not `static`, because the point of the ring is that somebody else empties
 * it. See `doom.log()` for the caller that exists today.
 */
size_t kosmos_spill_drain(char *out, size_t max)
{
    size_t n = 0;

    while (spill_tail != spill_head && n < max) {
        out[n++] = spill[spill_tail];
        spill_tail = (spill_tail + 1) % SPILL_MAX;
    }

    return n;
}

static int emit(const char *fmt, va_list ap)
{
    char line[PRINT_MAX];
    int n = vsnprintf(line, sizeof line, fmt, ap);

    if (n < 0) {
        return n;
    }

    /* vsnprintf returns what it *would* have written. What there is to
     * write is what fitted. */
    if ((size_t)n >= sizeof line) {
        n = (int)sizeof line - 1;
    }

    if (kosmos_write(line, (size_t)n) < 0) {
        spill_put(line, (size_t)n);
    }

    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = emit(fmt, ap);
    va_end(ap);

    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;

    (void)f;

    va_start(ap, fmt);
    n = emit(fmt, ap);
    va_end(ap);

    return n;
}

int vprintf(const char *fmt, va_list ap)
{
    return emit(fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;

    /*
     * No bound, because `sprintf` has none - that is what is wrong with it
     * and why nothing in Kosmos's own code may call it. It is here for
     * Doom, whose two uses write a fixed-length lump name into a buffer
     * sized for one. New code uses `snprintf`.
     */
    va_start(ap, fmt);
    n = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);

    return n;
}

int puts(const char *s)
{
    kosmos_write(s, strlen(s));
    kosmos_write("\n", 1);

    return 0;
}

int putchar(int c)
{
    char ch = (char)c;

    kosmos_write(&ch, 1);

    return c;
}

int fputs(const char *s, FILE *f)
{
    (void)f;

    kosmos_write(s, strlen(s));

    return 0;
}

int fputc(int c, FILE *f)
{
    (void)f;

    return putchar(c);
}

/*
 *--------------------------------------------------------------------------
 * The rest of the stream functions, which fail.
 *
 * These are the *savegame* half: seeking in a file, deleting one, renaming
 * one. Doom writes a temporary save and renames it over the old one, which
 * is the careful thing to do and needs three calls this system does not
 * have.
 *
 * They fail rather than being absent, and that is the useful shape: Doom
 * checks, prints what went wrong through the `fprintf` above, and carries
 * on without a save. A stub that *pretended* to succeed would lose the game
 * silently, which is worse than not saving.
 *
 * When saving matters, the answer is not to implement these - there is no
 * global tree for a path to mean anything in. It is for the game to hand
 * its bytes back the way it got them: through Lua, into the namespace this
 * process was given. Same division as the WAD.
 *--------------------------------------------------------------------------
 */

int fseek(FILE *f, long offset, int whence)
{
    long at;

    if (f == NULL || f->bytes == NULL) {
        return -1;
    }

    if (whence == SEEK_SET)      { at = offset; }
    else if (whence == SEEK_CUR) { at = (long)f->at + offset; }
    else if (whence == SEEK_END) { at = (long)f->len + offset; }
    else                         { return -1; }

    if (at < 0 || (size_t)at > f->len) {
        return -1;
    }

    f->at = (size_t)at;

    return 0;
}

long ftell(FILE *f)
{
    if (f == NULL || f->bytes == NULL) {
        return -1L;
    }

    return (long)f->at;
}

void rewind(FILE *f)
{
    if (f != NULL) {
        f->at = 0;
    }
}

int remove(const char *path)
{
    (void)path;

    return -1;
}

int rename(const char *from, const char *to)
{
    (void)from; (void)to;

    return -1;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    (void)f;

    return emit(fmt, ap);
}
